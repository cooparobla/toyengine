/**
 * @file terrain_component.h
 * @brief The scene-authored `TileTerrain` component (formerly `Terrain`, still accepted): what
 *        mapcoopa world to generate, which tile set to build it from, how to tile it, and the
 *        runtime state the streaming system drives.
 *
 * One of these on a SceneObject turns that object into the root of a streamed tile world. The
 * world is STORED (`tiles`, a TileMapData in a `.tiles` file): mapcoopa bakes it once from the
 * `generator:` settings, and from then on it is edited -- in the editor's Tile Mode, or by
 * gameplay -- and the generator only matters on a Regenerate. It
 * carries no per-frame logic of its own -- toy::world::TerrainSystem owns that (see
 * terrain_system.h for why the policy lives in a system rather than in update()). What lives
 * here is everything a scene author writes, plus the long-lived state that outlives any one
 * frame: the generator, the sampler, the baked tile library and the chunk table.
 *
 * Example YAML:
 * @code
 * - type: TileTerrain
 *   tile_set: objects/terrain/tileset_round    # THE template: tileset_voxel for blocks
 *   tiles: terrain/terrain.tiles               # the stored tiles (default terrain/<object>.tiles)
 *   kind_tile_sets: { stone: objects/terrain/tileset_rock }   # optional, per surface kind
 *   generator: { seed: 7, sea_level: 0.3 }    # mapcoopa keys; the rest is mapcoopa's standard map
 *   tiles_per_grid_unit: 4
 *   height_scale: 16.0
 *   water: { clarity: 2.0, foam_amount: 0.8 } # WaterBody keys for the sea, lakes and rivers
 *   chunk_size: 32
 *   view_radius: 2
 *   collision: true
 *   material:
 *     texture_albedo: textures/terrain_atlas.png
 *     texture_metallic_roughness: textures/terrain_atlas_mr.png
 * @endcode
 *
 * A **tile set** is an object asset (`tile_set: true`, assets/objects/terrain/tileset_*.yaml)
 * with one child per TilePiece, named by piece, each carrying the MeshRenderer for that piece
 * (see tile_topology.h). It is the terrain's whole shape: a rounded set and the square voxel
 * set go through the same mesher, so changing `tile_set` is all it takes to change the form.
 * The material (palette atlas) is separate. Paths resolve the ordinary way -- against the
 * loading scene's directory first, then the registered search roots.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_COMPONENT_H
#define TOYENGINE_WORLD_TERRAIN_COMPONENT_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/water/water_body.h>

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

    /** @brief The SceneObject carrying this chunk's Transform + MeshRenderer (+ MeshCollider), and its water
     *         children; null until Live. */
    coopa::scene::SceneObject* object = nullptr;

    /**
     * @brief Its tiles were edited since it was sampled: rebuild. A Live chunk keeps drawing
     *        while the rebuild meshes (it goes to Meshing with its object kept), and the new mesh
     *        replaces it in one frame -- no hole.
     */
    bool stale = false;

    /** @brief Bumped per upload; part of the published mesh's asset id, so a rebuild's new mesh never collides with the old one still in its grace period. */
    std::uint32_t mesh_serial = 0;
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
     * component's sampler and tile library (see terrain_chunk.h), so a job still queued or
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

    std::string type_name() const override { return "TileTerrain"; }

    // ------------------------------------------------------------------
    // Authored configuration -- everything a scene YAML sets.
    // ------------------------------------------------------------------

    /** @brief How the generated world is cut into tiles and chunks. */
    TerrainParams params;

    /**
     * @brief The mapcoopa settings a bake generates the world with -- AUTHORED values (see
     *        coopa::maps::derived_config(), applied at bake time).
     *
     * Starts as coopa::maps::standard_config(), the map a bare `mapcoopa` run makes, and the
     * scene's `generator:` block holds mapcoopa's own config keys for whatever differs
     * (map_yaml.h's names; older scenes' flat keys -- `seed`, `shape`, `sea_level`... -- still
     * load into it). Only a bake reads it: the tiles are the world from then on.
     */
    coopa::maps::MapConfig generator = coopa::maps::standard_config();

    // --- The tile set: the template every column is built from (see tile_topology.h) ---

    /** @brief The default tile set: an object asset path, e.g. `objects/terrain/tileset_round`. */
    std::string tile_set = k_default_tile_set;

    /** @brief Per-kind tile-set overrides, indexed by TileKind; empty = use `tile_set`. */
    std::array<std::string, k_tile_kind_count> kind_tile_sets;

    /** @brief The stock voxel (square block) tile set; also what a terrain without `tile_set` uses. */
    static constexpr const char* k_default_tile_set = "objects/terrain/tileset_voxel";

    /**
     * @struct StyleEntry
     * @brief One distinct tile set the terrain uses (the library's "style"), resolved by the
     *        parser from `tile_set` + `kind_tile_sets` (or the legacy `styles:` map).
     */
    struct StyleEntry {
        std::string name;                  /**< For log lines: the set's path or legacy style name. */
        std::string prefix;                /**< The tile-set object path (or a legacy mesh prefix). */
        bool        round_kind_corners = true; /**< The tile set's own `round_kind_corners`. */
    };

    /** @brief The distinct tile sets, in first-use order; style 0 is the default. */
    std::vector<StyleEntry> styles;

    /**
     * @brief The style each surface kind's cells are shaped with, by style index; kinds not
     *        listed take `default_style`.
     */
    std::array<int, k_tile_kind_count> kind_styles = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

    /** @brief The style for kinds `kind_styles` leaves unassigned (-1 entries). */
    int default_style = 0;

    // --- Collision ---

    /** @brief Give every live chunk a static MeshCollider of its own drawn surface. */
    bool collision = true;

    /** @brief physics_materials/<name>.yaml for the chunk colliders (optional). */
    std::string physics_material;

    /** @brief Material every chunk's MeshRenderer is given; the atlas lives here. */
    coopa::gfx::engine::components::PBRMaterial material;

    /**
     * @brief How the terrain's water looks and behaves -- the `water:` block, the same keys as a
     *        WaterBody (waves, foam, clarity, flow...). Every chunk's water surfaces copy it
     *        (water::copy_water_settings()); rivers take it with calmer waves.
     */
    water::WaterBody water_look = default_water_look();

    /** @brief The terrain's stock water look: small waves for a world of one-metre tiles. */
    static water::WaterBody default_water_look();

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

    // ------------------------------------------------------------------
    // The stored tiles (tile_map_data.h).
    // ------------------------------------------------------------------

    /** @brief The `.tiles` file, as authored (`tiles:`); default `terrain/<object>.tiles` beside the scene. */
    std::string tiles_file;

    /**
     * @brief The terrain's tiles: the terrain itself. Shared through TileMapStore, so re-parsing
     *        the component keeps the live (possibly unsaved) tiles. Set by the parser.
     *        Edit it -- write records, then mark_dirty() -- and the touched chunks re-mesh.
     */
    std::shared_ptr<TileMapData> tiles;

    /**
     * @brief Bakes the world when the tiles need it (first use with no file, or a Regenerate):
     *        generates the mapcoopa map off the calling thread, then fills the tiles from it.
     *        Idempotent; a terrain whose tiles loaded from a file never generates anything.
     *
     * @param jobs The shared JobEngine, or null to run inline on the calling thread.
     */
    void begin_generation(coopa::job::JobEngine* jobs);

    /**
     * @brief Advances a bake: builds the sampler from a finished map, runs the bake job, and
     *        hands the result to the tiles.
     *
     * @return True on the frame the baked tiles land.
     */
    bool poll_generation();

    /** @brief True while a bake (map generation + fill) is in flight. */
    bool baking() const { return generator_ != nullptr || bake_job_ != nullptr; }

    /** @brief True once there are tiles and the tile sets are baked -- i.e. chunks can build. */
    bool is_ready() const { return tiles && !tiles->empty() && library_.is_baked(); }

    /** @brief The map sampler from the last bake (built once a bake has run this session). */
    const TerrainSampler& sampler() const { return sampler_; }

    // --- Gameplay queries (world space; owner thread) ---

    /** @brief The tile under a world position, or false off the terrain. */
    bool tile_at(const glm::vec3& world, glm::ivec2& tile) const;
    /** @brief The stored record under a world position (biome, temperature...), or null. */
    const TileRecord* record_at(const glm::vec3& world) const;
    /** @brief World height of the ground under a world position (the bed, under water). */
    float height_at(const glm::vec3& world) const;

    /** @brief The tile revision TerrainSystem has applied to the chunk table. */
    std::uint64_t applied_tile_revision = 0;

    /** @brief The baked tile sets; read concurrently by chunk jobs once is_ready(). */
    const TileMeshLibrary& library() const { return library_; }

    /** @brief The chunk table, keyed by chunk coordinate. Owned by TerrainSystem. */
    std::unordered_map<ChunkCoord, TerrainChunk>& chunks() { return chunks_; }

    /** @brief The chunk table, for read-only inspection. */
    const std::unordered_map<ChunkCoord, TerrainChunk>& chunks() const { return chunks_; }

    /** @brief The asset handle for one tile-set piece; set by the "TileTerrain" scene parser. */
    void set_style_piece_source(std::size_t style, TilePiece piece,
                                coopa::asset::AssetHandle<SkinnedMeshSource> handle);

    /**
     * @brief Bakes the loaded tile-set pieces into the library, once every one has settled.
     *
     * Deliberately NOT done from a worker: an AssetHandle is a view onto a slot the
     * AssetManager mutates, so it is only safe to dereference on the owner thread. Everything
     * a chunk job touches is copied out of those handles here (see tile_mesh_library.h).
     *
     * @return True on the frame the library becomes baked; false while still waiting.
     */
    bool poll_tile_sets();

    /**
     * @brief Keeps a retired chunk's collision mesh alive a while: the physics world, or an
     *        async navmesh bake, may still read the shape it snapshotted. Owned by TerrainSystem.
     */
    void retire_collision(std::shared_ptr<const coopa::physx::geometry::TriangleMesh> mesh, std::uint64_t frame);

    /** @brief Drops retired collision meshes older than `frame - keep_frames`. */
    void release_retired_collision(std::uint64_t frame, std::uint64_t keep_frames);

private:
    /// Bakes every loaded piece and maps kinds onto styles. A piece that failed to load is
    /// reported once and left to TileMeshLibrary::finish_styles()'s fallbacks.
    void bake_styles_();

    coopa::debug::Logger logger_; /**< Named "terrain"; MapGenerator logs one line per stage. */
    coopa::job::JobEngine* jobs_ = nullptr; /**< Non-owning; only for draining jobs in the destructor. */
    bool warned_no_tile_set_ = false; /**< Latches the "tile set failed" message to one line. */

    coopa::maps::MapConfig                        map_config_;
    std::unique_ptr<coopa::maps::MapGenerator>    generator_; /**< Released once its graph is moved out. */
    coopa::maps::MapTask                          task_;      /**< Declared AFTER generator_ -- see begin_generation(). */

    /// The bake request (TileMapData::bake_requests()) last started, so each is baked once.
    std::uint64_t started_bake_request_ = ~std::uint64_t{0};
    /// The in-flight fill: the job writes `baked`, poll_generation() hands it to `tiles`.
    struct BakeJob {
        TileMapData baked;
    };
    std::shared_ptr<BakeJob>  bake_job_;
    coopa::job::JobHandle     bake_handle_;

    TerrainSampler  sampler_;
    TileMeshLibrary library_;

    std::vector<std::array<coopa::asset::AssetHandle<SkinnedMeshSource>, k_tile_piece_count>> style_sources_;

    std::unordered_map<ChunkCoord, TerrainChunk> chunks_;

    /// Collision meshes of retired chunks, with the frame they retired on (retire_collision()).
    std::deque<std::pair<std::uint64_t, std::shared_ptr<const coopa::physx::geometry::TriangleMesh>>> retired_collision_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_COMPONENT_H
