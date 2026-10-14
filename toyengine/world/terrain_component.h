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
 *   world_tiles: 512                          # the map rasterised to 512 x 512 tiles (0: cells x tiles_per_grid_unit)
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
#include <coopa/scene/scene_object.h>
#include <coopa/maps/map_presets.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/terrain_generator.h>
#include <toyengine/water/water_body.h>

namespace toy {
namespace world {

/**
 * @enum ChunkState
 * @brief Where a chunk is in the sample -> mesh -> upload -> draw pipeline.
 */
enum class ChunkState : std::uint8_t {
    Queued,   /**< @brief Wanted, not yet submitted (this frame's submissions were spent). */
    Meshing,  /**< @brief A job is building its geometry and its GPU mesh on a worker thread. */
    Live,     /**< @brief Published, in the scene tree and drawing. */
    Dormant,  /**< @brief Out of range but kept whole, detached from the tree (see TerrainChunk::parked). */
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

    /**
     * @brief A Dormant chunk's SceneObject, held off the tree rather than destroyed.
     *
     * The whole chunk survives in here -- its GPU mesh, its published asset (the MeshRenderer
     * still holds the handle, so nothing unloads), its water children and its collider -- so
     * coming back into range is an add_child() and nothing else. Null in every other state,
     * where `object` is owned by the terrain's SceneObject instead.
     */
    std::unique_ptr<coopa::scene::SceneObject> parked;

    /** @brief Frame this chunk was last in range; the Dormant set is evicted oldest first. */
    std::uint64_t last_used_frame = 0;

    /**
     * @brief Whether this chunk currently satisfies the collision request (`collision_radius`).
     *
     * True from the moment it is submitted with collision on -- including for an empty chunk,
     * which satisfies the request with no collider because it has no geometry -- and false again
     * once the collider has been taken away. Comparing it with what the radius wants is what
     * decides between a rebuild (inward) and a component removal (outward), and reading it as
     * "has a MeshCollider" instead would make an empty chunk rebuild itself for ever.
     */
    bool has_collision = false;

};

/**
 * @struct StreamCentre
 * @brief The latched chunk the loaded region is centred on.
 *
 * The centre must be a *step* function of where the camera is, not a continuous one. The
 * candidate it is fed moves every frame -- a camera viewing the terrain from afar is streamed
 * around what it LOOKS at, and that point sweeps tens of metres across the ground for a few
 * degrees of yaw. Following it directly drops and rebuilds whole rings for a camera that never
 * moved. See latch_stream_centre().
 */
struct StreamCentre {
    ChunkCoord   centre{};          /**< What the region is centred on now. */
    ChunkCoord   candidate{};       /**< The candidate `held` frames of agreement are counting. */
    std::int32_t held  = 0;         /**< Consecutive frames `candidate` has been asked for. */
    bool         valid = false;     /**< False until the first centre is adopted. */
};

/** @brief Frames a new candidate must hold before the region moves to it (see StreamCentre). */
inline constexpr std::int32_t k_recentre_frames = 6;

/**
 * @brief Advances a latched stream centre by one frame and returns the centre to stream around.
 *
 * Pure, so the policy can be tested without a camera. Three cases, in order:
 *
 *  - **No centre yet**, or the candidate is half a view away (a teleport, a scene load, or just
 *    a camera moving faster than the latch): adopt it at once. Past half the radius the region
 *    no longer comfortably covers the camera, and lagging further would mean holes ahead.
 *  - **The candidate is the current centre**: nothing to do, and the count resets -- so a
 *    candidate that flickers between two chunks never accumulates a move.
 *  - **Anything else**: adopt only once the same candidate has been asked for
 *    `k_recentre_frames` consecutive frames.
 *
 * @param state       The latch, advanced in place.
 * @param candidate   The chunk this frame would like to centre on.
 * @param view_radius The loaded radius, in chunks; half of it is the jump taken at once.
 * @return The centre to stream around this frame.
 */
inline ChunkCoord latch_stream_centre(StreamCentre& state, const ChunkCoord& candidate,
                                      std::int32_t view_radius) {
    const std::int32_t jump = std::max<std::int32_t>(1, view_radius / 2);
    if (!state.valid || chunk_distance(candidate, state.centre) >= jump) {
        state.centre    = candidate;
        state.candidate = candidate;
        state.held      = 0;
        state.valid     = true;
        return state.centre;
    }
    if (candidate == state.centre) {
        state.candidate = candidate;
        state.held      = 0;
        return state.centre;
    }
    if (candidate == state.candidate) {
        if (++state.held >= k_recentre_frames) {
            state.centre = candidate;
            state.held   = 0;
        }
    } else {
        state.candidate = candidate;
        state.held      = 1;
    }
    return state.centre;
}

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
     * Starts as default_generator() -- the map a bare `mapcoopa` run makes, in mapcoopa's natural
     * terrain model -- and the
     * scene's `generator:` block holds mapcoopa's own config keys for whatever differs
     * (map_yaml.h's names; older scenes' flat keys -- `seed`, `shape`, `sea_level`... -- still
     * load into it). Only a bake reads it: the tiles are the world from then on.
     */
    coopa::maps::MapConfig generator = default_generator();

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

    /**
     * @brief Planet only (`params.planet`): the pull at the surface, m/s^2, toward the planet's
     *        centre -- a physics gravity source TerrainSystem keeps there. 0 adds none.
     */
    float gravity = 9.81f;

    /** @brief The terrain's stock water look: small waves for a world of one-metre tiles. */
    static water::WaterBody default_water_look();

    /**
     * @brief Chunk meshing jobs submitted per frame.
     *
     * A pacing knob, not a limit on parallelism: `max_chunks_in_flight` is what actually bounds
     * how much work is outstanding, and the jobs themselves run on every worker.
     */
    int max_chunk_jobs_per_frame = 16;

    /**
     * @brief Chunks allowed to be meshing at once; 0 means twice the JobEngine's worker count.
     *
     * This, rather than a per-frame submission count, is what keeps a teleport from queueing the
     * whole world at once -- and what keeps the worker pool fed, which a small per-frame count
     * cannot do: four submissions a frame leave a twenty-worker engine idle while the camera
     * waits two seconds for its ground.
     */
    int max_chunks_in_flight = 0;

    /**
     * @brief Owner-thread milliseconds a frame may spend publishing finished chunks.
     *
     * A time slice, because the work is not per chunk: attaching a chunk costs a scene insert, a
     * published asset, its water children's first bake and (near the camera) a physics
     * re-gather, and those differ by an order of magnitude between a chunk of open sea and a
     * chunk of cliff. At least one chunk always lands, so the stream can never stall.
     *
     * Wall clock, so how many chunks a *transient* (a teleport, a first load) spreads over how
     * many frames is machine-dependent. The settled region is not: the same chunks end up
     * loaded either way, so a scripted capture that waits for the world to come up is as
     * reproducible as it ever was.
     */
    float upload_budget_ms = 1.5f;

    /**
     * @brief Chunks within which a chunk is given a MeshCollider; 0 means every loaded chunk.
     *
     * Collision is the most expensive thing a chunk carries: a welded triangle mesh with
     * adjacency and a BVH, a megabyte or two of it, and a `PhysicsSystem::refresh()` whose
     * re-gather is linear in every collider in the scene. Ground a few hundred metres away
     * pays all of that for contacts that never happen. Raise it past where your physics
     * actually is -- a body resting on a chunk outside this radius has no ground.
     */
    int collision_radius = 4;

    /**
     * @brief Metres past which a chunk stops casting shadows (MeshRenderer::shadow_max_distance);
     *        0 leaves every loaded chunk in every cascade. The same knob HeightmapTerrain has.
     */
    float shadow_distance = 0.0f;

    /**
     * @brief Chunks kept whole after leaving view, to be re-attached if the camera comes back.
     *
     * Turning round at a chunk boundary, or walking back ten metres, otherwise re-meshes,
     * re-uploads, re-bakes the water and rebuilds the collider BVH of ground that was complete
     * a moment ago. A parked chunk costs its GPU mesh (a megabyte or two); 0 disables the tier.
     */
    int dormant_chunks = 32;

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

    /** @brief The latched centre of the loaded region (see latch_stream_centre()). */
    StreamCentre stream_centre;

    /**
     * @brief A build job to fill in, from the pool if one is free.
     *
     * Pooled for the capacity, not the object: a chunk's pad, vertex and index buffers are a
     * couple of megabytes between them, and a streaming camera submits several chunks a frame.
     */
    std::shared_ptr<ChunkBuildJob> take_build_job();

    /**
     * @brief Offers a finished job back to the pool. Keeps it only if nothing else holds a
     *        reference -- a cancelled job may still be running, and its buffers are its own.
     */
    void recycle_build_job(std::shared_ptr<ChunkBuildJob> job);

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

    /// Free build jobs, kept for their buffers' capacity (take_build_job()).
    std::vector<std::shared_ptr<ChunkBuildJob>> job_pool_;
    /// Jobs the pool holds at most; past this a returned job is simply dropped.
    static constexpr std::size_t k_job_pool_max = 8;

    /// Collision meshes of retired chunks, with the frame they retired on (retire_collision()).
    std::deque<std::pair<std::uint64_t, std::shared_ptr<const coopa::physx::geometry::TriangleMesh>>> retired_collision_;
};

/**
 * @brief Whether the chunk at `coord` should carry a MeshCollider, with the region centred on
 *        `centre` (see TerrainComponent::collision_radius).
 *
 * A planet has no radius: it is small, it is walked all over, and a ring there would only mean
 * ground that is solid in front of you and not behind.
 *
 * @param terrain The terrain, for `collision` and `collision_radius`.
 * @param coord   The chunk in question.
 * @param centre  The chunk the loaded region is centred on.
 */
bool terrain_wants_collision(const TerrainComponent& terrain, const ChunkCoord& coord,
                             const ChunkCoord& centre);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_COMPONENT_H
