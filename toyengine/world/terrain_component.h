/**
 * @file terrain_component.h
 * @brief The scene-authored `Terrain` component: what world to generate, how to tile it, and
 *        the runtime state the streaming system drives.
 *
 * One of these on a SceneObject turns that object into the root of a streamed tile world. It
 * carries no per-frame logic of its own -- toy::world::TerrainSystem owns that (see
 * terrain_system.h for why the policy lives in a system rather than in update()). What lives
 * here is everything a scene author writes, plus the long-lived state that outlives any one
 * frame: the generator, the sampler, the baked side library and the chunk table.
 *
 * Example YAML:
 * @code
 * - type: Terrain
 *   seed: 251
 *   grid_size: 64
 *   tiles_per_grid_unit: 4
 *   height_scale: 40.0
 *   chunk_size: 16
 *   view_radius: 3
 *   side_mesh: tile_side_flat            # the canonical +Z side, used for all six faces
 *   sides: { top: tile_side_bevel }      # ...optionally overriding individual faces
 *   material:
 *     albedo: { r: 1.0, g: 1.0, b: 1.0 }
 *     roughness: 0.9
 *     texture_albedo: textures/terrain_atlas.png
 * @endcode
 *
 * Both `side_mesh`/`sides` and the material's `texture_*` paths resolve the ordinary way --
 * against the loading scene's own directory first, then the registered search roots (see
 * coopa::asset::AssetSource::resolve()). The stock tile set ships in the SHARED assets/meshes/
 * and assets/textures/ rather than inside any one scene, so a scene names it without copying it,
 * and overrides it by placing a file of the same name beside its own scene.yaml.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_COMPONENT_H
#define TOYENGINE_WORLD_TERRAIN_COMPONENT_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/maps/map_generator.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/world/terrain_chunk.h>

namespace toy {
namespace world {

/**
 * @enum ChunkState
 * @brief Where a chunk is in the sample -> mesh -> upload -> draw pipeline.
 */
enum class ChunkState : std::uint8_t {
    Queued,   /**< @brief Wanted, not yet submitted (the per-frame job budget was spent). */
    Meshing,  /**< @brief A job is building its geometry on a worker thread. */
    Live,     /**< @brief Uploaded, published and drawing. */
    Retiring  /**< @brief Out of range; waiting for its job and the GPU to let go. */
};

/**
 * @struct TerrainChunk
 * @brief One chunk's lifetime state, from "wanted" to "drawing" to "let go of".
 */
struct TerrainChunk {
    ChunkCoord   coord;
    ChunkState   state = ChunkState::Queued;

    /** @brief The meshing job's group handle. Valid only in Meshing; must be close()d exactly once. */
    coopa::job::JobHandle job;

    /**
     * @brief The job's inputs and output buffer -- see ChunkBuildJob.
     *
     * A shared_ptr, and captured BY VALUE into the job, so a chunk retired mid-flight leaves the
     * job writing into a buffer that outlives the chunk entry rather than into freed memory.
     */
    std::shared_ptr<ChunkBuildJob> build;

    /** @brief The GPU mesh, kept alive here (an AssetHandle only ever hands out a const Mesh*). */
    std::shared_ptr<coopa::gfx::engine::data::Mesh> mesh;

    /** @brief The SceneObject carrying this chunk's Transform + MeshRenderer; null until Live. */
    coopa::scene::SceneObject* object = nullptr;
};

/**
 * @class TerrainComponent
 * @brief Scene-authored terrain configuration, and the world state built from it.
 */
class TerrainComponent : public coopa::scene::Component {
public:
    using SkinnedMeshSource = coopa::gfx::engine::data::SkinnedMeshSource;

    TerrainComponent() : logger_("terrain") {}

    /**
     * @brief Cancels and WAITS FOR every in-flight chunk job before letting anything go.
     *
     * Not optional, and not merely tidy. A ChunkBuildJob borrows raw pointers to this
     * component's sampler and side library (see terrain_chunk.h), so a job still queued or
     * running when this object is destroyed would dereference freed memory -- the classic
     * shutdown-race that only ever reproduces on a fast machine. Waiting here is what makes
     * "the sampler outlives every job that reads it" true rather than aspirational.
     *
     * Closing the handles matters too: coopa::job::JobEngine asserts at shutdown that every
     * handle it handed out was closed, so a scene torn down mid-stream would otherwise end in
     * a leaked-slot warning.
     *
     * Safe by construction in toy::core::Engine, whose `jobs_` is declared first and therefore
     * destroyed last -- after the SceneManager that owns this component.
     */
    ~TerrainComponent() override;

    std::string type_name() const override { return "Terrain"; }

    // ------------------------------------------------------------------
    // Authored configuration -- everything a scene YAML sets.
    // ------------------------------------------------------------------

    /** @brief How the generated world is cut into tiles and chunks. */
    TerrainParams params;

    /** @brief Master seed; the same seed always regenerates the same world. */
    int seed = 251;

    /** @brief Map cells per axis. The world is `grid_size * tiles_per_grid_unit` tiles square. */
    int grid_size = 64;

    /** @brief Waterline, as a fraction of the elevation range. Land occupies `[sea_level, 1]`. */
    double sea_level = 0.25;

    /** @brief Fractal displacement amplitude over the control mesh; 0 leaves the terrain faceted. */
    double terrain_roughness = 0.35;

    /** @brief River sources to attempt. Rivers cut visible channels into the tiled surface. */
    int river_count = 25;

    // --- The landmass outline (mapcoopa ShapeConfig) ---
    // Distances are mapcoopa's physical metres (against its 60 m grid unit by default), NOT
    // world units: they shape the map, and `height_scale`/`tile_size` then size the tiles.

    /** @brief `rectangle` (fills the canvas), `circle`, `triangle`, `continent` or `archipelago`. */
    std::string shape = "rectangle";

    /** @brief Archipelago only: landmasses to place. */
    int continent_count = 4;

    /** @brief Continent/archipelago: mean landmass diameter in metres; 0 sizes it to the canvas. */
    double continent_size_m = 0.0;

    /** @brief How far a landmass outline wanders from a circle, `[0, 0.6]`. */
    double irregularity = 0.35;

    /** @brief Fine coastline noise amplitude. */
    double coast_detail = 0.12;

    /**
     * @brief Added to the whole map's temperature, roughly `[-0.5, 0.5]`: negative cools hot
     *        deserts and savanna toward temperate grassland and forest.
     */
    double temperature_offset = 0.0;

    // --- Styled tiles (see tile_topology.h) ---

    /**
     * @struct StyleEntry
     * @brief One named tile style and the mesh prefix its pieces load from:
     *        `meshes/<prefix>_<piece>.yaml`, one file per TilePiece.
     */
    struct StyleEntry {
        std::string name;
        std::string prefix;
    };

    /**
     * @brief The scene's tile styles. Empty keeps the voxel look; any entry switches every
     *        chunk to the styled mesher.
     */
    std::vector<StyleEntry> styles;

    /**
     * @brief The style each surface kind's cells are shaped with, by style index; kinds not
     *        listed take `default_style`.
     */
    std::array<int, k_tile_kind_count> kind_styles = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    /** @brief The style for kinds `kind_styles` leaves unassigned (-1 entries). */
    int default_style = 0;

    /** @brief Logical path of the canonical (+Z) side mesh, relative to the scene's meshes/ dir. */
    std::string side_mesh = "tile_side_flat";

    /** @brief Optional per-face overrides of `side_mesh`, indexed by `TileFace`; empty = inherit. */
    std::array<std::string, k_tile_face_count> face_meshes;

    /** @brief Material every chunk's MeshRenderer is given; the atlas lives here. */
    coopa::gfx::engine::components::PBRMaterial material;

    /**
     * @brief Chunk meshing jobs submitted per frame.
     *
     * A budget, not a limit on parallelism: the jobs themselves run on every worker. It bounds
     * the main thread's per-frame *upload* cost instead, so a camera teleport spreads its
     * rebuild over a handful of frames rather than stalling one.
     */
    int max_chunk_jobs_per_frame = 4;

    // ------------------------------------------------------------------
    // Runtime state -- driven by TerrainSystem, not by scene YAML.
    // ------------------------------------------------------------------

    /**
     * @brief Starts generating the world, off the calling thread when an engine is available.
     *
     * Idempotent: a second call while generation is in flight (or after it finished) does
     * nothing. Generation is ~135 ms for a default map (see MapGenerator's own note), which is
     * why it is not done inline in the component's constructor.
     *
     * @param jobs The shared JobEngine, or null to generate inline on the calling thread.
     */
    void begin_generation(coopa::job::JobEngine* jobs);

    /**
     * @brief Moves a finished generation into the sampler, once.
     *
     * @return True on the frame the sampler becomes usable; false otherwise.
     */
    bool poll_generation();

    /** @brief True once the world exists and the side meshes are baked -- i.e. chunks can build. */
    bool is_ready() const { return sampler_.is_built() && library_.is_baked(); }

    /** @brief The built world sampler; read concurrently by chunk jobs once is_ready(). */
    const TerrainSampler& sampler() const { return sampler_; }

    /** @brief The baked side geometry; read concurrently by chunk jobs once is_ready(). */
    const TileMeshLibrary& library() const { return library_; }

    /** @brief The chunk table, keyed by chunk coordinate. Owned by TerrainSystem. */
    std::unordered_map<ChunkCoord, TerrainChunk>& chunks() { return chunks_; }

    /** @brief The chunk table, for read-only inspection. */
    const std::unordered_map<ChunkCoord, TerrainChunk>& chunks() const { return chunks_; }

    /** @brief The asset handle for the canonical side mesh; set by the "Terrain" scene parser. */
    void set_side_source(coopa::asset::AssetHandle<SkinnedMeshSource> handle) {
        side_source_ = std::move(handle);
    }

    /** @brief The asset handle for one face's override mesh; set by the "Terrain" scene parser. */
    void set_face_source(TileFace face, coopa::asset::AssetHandle<SkinnedMeshSource> handle) {
        face_sources_[static_cast<std::size_t>(face)] = std::move(handle);
    }

    /** @brief The asset handle for one styled piece; set by the "Terrain" scene parser. */
    void set_style_piece_source(std::size_t style, TilePiece piece,
                                coopa::asset::AssetHandle<SkinnedMeshSource> handle);

    /**
     * @brief Bakes the loaded side meshes into the library, once they have finished loading.
     *
     * Deliberately NOT done from a worker: an AssetHandle is a view onto a slot the
     * AssetManager mutates, so it is only safe to dereference on the owner thread. Everything
     * a chunk job touches is copied out of those handles here (see tile_mesh_library.h).
     *
     * @return True on the frame the library becomes baked; false while still waiting.
     */
    bool poll_side_meshes();

private:
    /// Bakes every loaded styled piece and maps kinds onto styles. A piece that failed to load
    /// is reported once and left to TileMeshLibrary::finish_styles()'s fallbacks.
    void bake_styles_();

    coopa::debug::Logger logger_; /**< Named "terrain"; MapGenerator logs one line per stage. */
    coopa::job::JobEngine* jobs_ = nullptr; /**< Non-owning; only for draining jobs in the destructor. */
    bool warned_side_mesh_ = false; /**< Latches the "side mesh failed" message to one line. */

    coopa::maps::MapConfig                        map_config_;
    std::unique_ptr<coopa::maps::MapGenerator>    generator_; /**< Released once its graph is moved out. */
    coopa::maps::MapTask                          task_;      /**< Declared AFTER generator_ -- see begin_generation(). */

    TerrainSampler  sampler_;
    TileMeshLibrary library_;

    coopa::asset::AssetHandle<SkinnedMeshSource> side_source_;
    std::array<coopa::asset::AssetHandle<SkinnedMeshSource>, k_tile_face_count> face_sources_;
    std::vector<std::array<coopa::asset::AssetHandle<SkinnedMeshSource>, k_tile_piece_count>> style_sources_;

    std::unordered_map<ChunkCoord, TerrainChunk> chunks_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_COMPONENT_H
