/**
 * @file terrain_system.h
 * @brief Streams terrain chunks around the camera: which chunks exist, who meshes them, and
 *        when their GPU meshes are created and let go.
 *
 * ### Why a system and not a component method
 *
 * Only `ISceneSystem::execute()` receives a `FrameContext`, and this needs two things that live
 * on it: `ctx.jobs`, the shared `coopa::job::JobEngine` chunk meshing fans out across, and the
 * guarantee that it is running on the Scene's owner thread outside the component walk -- which
 * is what makes it safe to add and remove SceneObjects directly (see
 * coopa/scene/scene_system.h's FrameContext::commands doc). A `Component::update()` has neither.
 *
 * Installed at order 60: inside `Scene::update()`, ahead of `Physics` (100) and the `Behaviour`
 * walk (200), so a chunk that appears this frame is already in the tree when
 * `UpdatePhase::TransformResolve` (350) resolves its world matrix and the render gather reads it.
 * Same insertion technique as toy::scene::KinematicControlSystem at order 50.
 *
 * ### The thread split
 *
 * Off-thread (any worker): assembling tile-set pieces into vertex/index buffers, creating and
 * filling the chunk's GPU mesh, and building its collision mesh (weld, adjacency, BVH) -- see
 * terrain_chunk.h for why the Vulkan half is safe there. The chunk's columns are copied out of
 * the terrain's tiles on the owner thread when the job is submitted (a plain copy), so the tiles
 * can be edited at any time; an edit marks the chunks it touches stale, and each rebuilds while
 * its old mesh keeps drawing, then swaps in. That is nearly all the cost, and chunks are
 * independent.
 *
 * Owner thread only: publishing the finished mesh as an asset and touching the scene tree.
 * `AssetManager` is single-owner and a Scene is single-owner -- neither may be reached from a
 * job. That is what `upload_budget_ms` bounds, and it is all that is left in the frame a chunk
 * lands in.
 *
 * ### What is built, and where
 *
 * Chunks are submitted nearest-first and at a priority that falls off with distance, so the
 * ground under the camera is meshed before the horizon however the queue was ordered, and the
 * far ring never competes with frame-critical jobs. How much is outstanding is bounded by
 * `max_chunks_in_flight` rather than by a per-frame count, so the worker pool stays fed.
 * `collision_radius` keeps the collider -- the most expensive thing a chunk carries -- near the
 * camera, where contacts actually happen.
 *
 * ### Letting go safely
 *
 * Two independent hazards, handled separately rather than with one conservative delay:
 *
 *   - **A still-running job.** Its `ChunkBuildJob` is shared-owned, so dropping the streamer's
 *     reference cannot pull the buffer out from under it. What must NOT be dropped early is the
 *     JobHandle: every handle has to be `close()`d exactly once (see coopa/job/handle.h), so a
 *     retiring chunk with a job in flight is cancelled and parked in `Retiring` until
 *     `is_complete()`, then closed and erased. Scene teardown is the one case this cannot cover
 *     -- a job borrows the component's sampler -- and `~TerrainComponent()` drains the rest.
 *   - **A chunk that may be wanted again in a moment.** Parked, not destroyed: up to
 *     `dormant_chunks` of them are kept whole and off the scene tree (TerrainChunk::parked), so
 *     turning round at a chunk boundary re-attaches ground that is already built instead of
 *     re-meshing, re-uploading and re-baking it.
 *   - **A still-in-flight GPU read.** Handled by the AssetManager's own payload grace period
 *     (`k_payload_grace_frames`): destroying the chunk's SceneObject drops the last
 *     AssetHandle, `unload()` then retires the payload for three more `assets.update()` calls
 *     before it is actually destroyed. That is the same mechanism a hot reload uses.
 *   - **A still-referenced collision shape.** The physics world holds a raw pointer to a
 *     chunk's TriangleMesh until its next refresh(), and an async navmesh bake may hold its
 *     snapshot longer. A retired chunk's mesh is therefore parked on the component
 *     (TerrainComponent::retire_collision()) for k_collision_keep_frames before it is freed.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_SYSTEM_H
#define TOYENGINE_WORLD_TERRAIN_SYSTEM_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <coopa/scene/scene.h>

#include <gfxcoopa/engine/components/camera_component.h>

#include <toyengine/world/terrain_component.h>

namespace toy {
namespace world {

/** @brief Default registration order: inside update(), ahead of `UpdatePhase::Physics` (100). */
inline constexpr int k_terrain_system_order = 60;

/**
 * @class TerrainSystem
 * @brief Drives every TerrainComponent in the scene: generate, mesh, upload, retire.
 */
class TerrainSystem : public coopa::scene::ISceneSystem {
public:
    using Mesh = coopa::gfx::engine::data::Mesh;
    using MeshRenderer = coopa::gfx::engine::components::MeshRenderer;
    using CameraComponent = coopa::gfx::engine::components::CameraComponent;
    using TransformComponent = coopa::scene::TransformComponent;

    /**
     * @param device    Vulkan logical device, for each chunk's vertex/index buffers.
     * @param allocator VMA allocator.
     * @param assets    Asset manager the runtime chunk meshes are published into.
     */
    TerrainSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  coopa::asset::AssetManager& assets)
        : device_(device), allocator_(allocator), assets_(assets) {}

    const char* system_name() const override { return "Terrain"; }
    /// Terrain is authored data, not simulation: an editor scene still meshes it.
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /** @brief Chunk meshes published since installation (a revived parked chunk is not one). */
    std::uint64_t chunks_built() const { return chunks_built_; }
    /** @brief Chunks destroyed since installation (a parked chunk is not one until evicted). */
    std::uint64_t chunks_released() const { return chunks_released_; }
    /** @brief Owner-thread milliseconds the last execute() spent publishing chunks. */
    double last_upload_ms() const { return last_upload_ms_; }

    /**
     * @brief Chunks wanted at the current stream centre, and how many are not drawing yet.
     *
     * Both are the previous execute()'s figures, counted by want_chunks_() as it walks the
     * wanted square. `unfinished` counts a chunk that is queued, meshing or retiring, and a
     * published one whose tiles have been edited since (stale, so due to re-mesh).
     */
    std::int32_t wanted_chunks() const { return wanted_chunks_; }
    std::int32_t unfinished_chunks() const { return unfinished_chunks_; }

    /**
     * @brief Not warm until every wanted chunk is drawing -- a streamed world's ground.
     *
     * Reports unwarm while a terrain is still baking its world or its tile set too: nothing has
     * been asked for at that point, and a zero outstanding count would otherwise read as done.
     */
    coopa::scene::WarmupState warm_up_state() const override {
        coopa::scene::WarmupState ws;
        ws.participates = terrain_baking_ || wanted_chunks_ > 0;
        if (!ws.participates) return ws;
        ws.what = "terrain";
        ws.warm = !terrain_baking_ && unfinished_chunks_ == 0;
        ws.remaining = unfinished_chunks_;
        ws.progress = wanted_chunks_ > 0
                          ? static_cast<float>(wanted_chunks_ - unfinished_chunks_) /
                                static_cast<float>(wanted_chunks_)
                          : 0.0f;
        return ws;
    }

private:
    /**
     * @brief Follows AssetManager::k_asset_io_job_type's precedent: any value >= k_max_job_types
     *        (64) opts out of thread dedication and diagnostics counters, which is what a batch
     *        of independent, dependency-free jobs wants.
     */
    static constexpr coopa::job::JobType k_terrain_job_type = 0x7E44A100u;

    std::int32_t wanted_chunks_ = 0;      /**< See wanted_chunks(). */
    std::int32_t unfinished_chunks_ = 0;  /**< See unfinished_chunks(). */
    bool terrain_baking_ = false;         /**< Any terrain still baking its world or tile set. */

    /** @brief One terrain's whole frame: generate, then stream. */
    void tick_terrain_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                       const coopa::scene::FrameContext& ctx);

    /**
     * @brief The point this frame would like the loaded region centred on, in the terrain's
     *        local space: the camera's own position while it stands over the loaded region, and
     *        otherwise where its view ray meets the tiles (or the terrain's base plane) -- so an
     *        orbit or editor camera looking at the terrain from off the map still loads what it
     *        looks at. The terrain's origin without a camera, so a headless test still builds
     *        the chunk under it.
     *
     * Preferring the camera's POSITION where it is usable is the whole point: the ray's hit
     * point sweeps across the ground as the camera turns, and it is only a better answer than
     * the camera itself when the camera is nowhere near the ground. What survives the remaining
     * movement is latch_stream_centre().
     *
     * get_world_matrix(), not world_matrix(): this system runs at order 60, BEFORE
     * UpdatePhase::TransformResolve (350), so on the first frame no resolve pass has run yet and
     * the non-resolving read would trip its own debug assert. Resolving here is safe because
     * this is the owner thread and nothing else is reading concurrently.
     */
    glm::vec3 stream_centre_(const TerrainComponent& terrain) const;

    /** @brief The chunk to stream around this frame: stream_centre_(), latched (StreamCentre). */
    ChunkCoord latched_centre_(TerrainComponent& terrain) const;

    /** @brief True when any part of this chunk lies inside the generated map. */
    static bool chunk_in_world(const TerrainComponent& terrain, const ChunkCoord& coord);

    /**
     * @brief Brings the in-range chunks up to date: a Queued entry for every one that does not
     *        exist yet, a re-attach for every parked one, and a collider for every one that has
     *        come inside `collision_radius`.
     */
    void want_chunks_(TerrainComponent& terrain, const ChunkCoord& centre, coopa::scene::Scene& scene);

    /**
     * @brief Parks or drops chunks that have left the loaded region.
     *
     * The threshold is `view_radius + 1`, not `view_radius`: a camera sitting exactly on a chunk
     * boundary otherwise oscillates a whole ring of chunks between built and dropped every time
     * it drifts a few centimetres, which is the most expensive thing this system can do. Past
     * it, a Live chunk goes Dormant (kept whole, off the tree) rather than being destroyed.
     */
    void release_distant_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                                 const coopa::scene::FrameContext& ctx);

    /** @brief Re-attaches a Dormant chunk that has come back into range. */
    void revive_chunk_(TerrainComponent& terrain, coopa::scene::Scene& scene, TerrainChunk& chunk);

    /** @brief Destroys Dormant chunks past `dormant_chunks`, oldest first. */
    void evict_dormant_chunks_(TerrainComponent& terrain);

    /**
     * @brief Brings a Live chunk's collider in line with `collision_radius`.
     *
     * Crossing inward needs geometry the chunk was not built with, so it raises
     * TerrainChunk::wants_collision and a collider-only job supplies it -- the chunk goes on
     * drawing what it already has throughout. Crossing outward only has to let the collider go,
     * which is a component removal and a retired shape.
     */
    void sync_chunk_collision_(TerrainComponent& terrain, TerrainChunk& chunk, bool wanted);

    /**
     * @brief Hangs a finished collider-only build on the object the chunk is already drawing.
     *
     * Everything else about the chunk is left alone -- that is the point of the pass. Drops the
     * shape instead when the chunk left `collision_radius` again while the job ran.
     */
    void attach_chunk_collision_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                                 TerrainChunk& chunk);


    /** @brief Closes and erases retiring chunks whose in-flight job has finally stopped. */
    void advance_retiring_(TerrainComponent& terrain);

    /**
     * @brief Publishes chunks whose meshing jobs have finished, nearest first, within
     *        `upload_budget_ms`.
     *
     * Nearest first and not in hash order: the chunk under the camera must not wait behind the
     * horizon's. A time slice rather than a count because what is left on this thread -- a
     * scene insert, a published asset, the water children's first bake, a physics re-gather --
     * differs by an order of magnitude between a chunk of open sea and a chunk of cliff. One
     * chunk always lands, whatever the budget.
     */
    void upload_finished_chunks_(TerrainComponent& terrain, coopa::scene::Scene& scene);

    /**
     * @brief Submits meshing jobs for the nearest queued chunks, up to `max_chunks_in_flight`.
     *
     * Each is submitted at a priority that falls off with distance from the centre, so the
     * JobEngine works the ground under the camera first whatever order the queue was in.
     */
    void submit_queued_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                               const coopa::scene::FrameContext& ctx);

    /**
     * @brief Creates the GPU mesh, publishes it, and hangs a drawable (and collidable) child off
     *        the terrain.
     *
     * `kept_water`, when not empty, is this chunk's existing water children, taken off the old
     * object by take_chunk_water_(): they are re-parented onto the new one instead of fresh
     * WaterBodies being built, so a rebuild keeps surfaces that are already baked and drawing.
     */
    void create_chunk_object_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                              TerrainChunk& chunk,
                              std::vector<std::unique_ptr<coopa::scene::SceneObject>> kept_water = {});

    /**
     * @brief Takes a chunk's water children off its object, still baked, for the rebuild
     *        replacing it; empty when the chunk has no object or no water.
     *
     * Only sound when the new build's water is identical -- see TerrainChunk::water_hash.
     */
    std::vector<std::unique_ptr<coopa::scene::SceneObject>> take_chunk_water_(TerrainChunk& chunk);

    /** @brief Takes a chunk's SceneObject off the tree, retiring its collider; keeps the object. */
    std::unique_ptr<coopa::scene::SceneObject> detach_chunk_object_(TerrainComponent& terrain,
                                                                    TerrainChunk& chunk);

    /** @brief Destroys a chunk's SceneObject and retires its published mesh. */
    /** @brief Gives a chunk object one water child (still or flowing) for its water surface. */
    void add_chunk_water_(const TerrainComponent& terrain, coopa::scene::SceneObject& chunk_object,
                          const ChunkWaterData& water, bool flowing);
    void destroy_chunk_object_(TerrainComponent& terrain, TerrainChunk& chunk);

    /** @brief Synthetic asset id for a chunk's current mesh, prefixed to stay out of the real-path namespace. */
    static std::string chunk_asset_id_(const TerrainComponent& terrain, const TerrainChunk& chunk);

    /**
     * @brief Marks every chunk whose tiles (or skirt) were edited since the last frame stale, so
     *        submit_queued_chunks_() rebuilds it -- nearest first, like new chunks.
     */
    void mark_edited_chunks_(TerrainComponent& terrain);

    /**
     * @brief Spends this frame's budget on the terrain's water settles (TerrainComponent::
     *        water_settles), so a flood started by an edit crosses the world over several
     *        frames instead of in one.
     *
     * Runs before mark_edited_chunks_(), so the tiles it fills stale their chunks in the same
     * frame and the rebuilds ride the streamer's existing pacing.
     */
    void advance_water_settles_(TerrainComponent& terrain);

    /** @brief A planet keeps every chunk: all six faces are always wanted. */
    void want_planet_chunks_(TerrainComponent& terrain);
    /**
     * @brief Hides the chunks of a planet that the planet itself is in front of.
     *
     * The far side of a globe is INSIDE the camera's frustum -- the pipeline's per-view frustum
     * test (`gather_meshes_()`) cannot reject it, and without this roughly half of every planet
     * is drawn, shadow-mapped and water-simulated behind its own horizon. A chunk is hidden by
     * deactivating its SceneObject, which prunes it and its water children from the render
     * snapshot (`ToyRenderPipeline::snapshot_scene_()`) and from the scene's update walk; the
     * chunk itself stays loaded, so coming back round the limb costs nothing.
     *
     * Only chunks past the horizon by a margin are hidden -- the margin covers the chunk's own
     * angular size and the height of what stands on it -- and the threshold to hide is further
     * out than the threshold to show again, so a chunk at the limb cannot flicker.
     */
    void cull_planet_horizon_(TerrainComponent& terrain);
    /** @brief Keeps a planet's gravity source in the physics world, at its centre (and drops gone ones). */
    void sync_planet_gravity_(coopa::scene::Scene& scene);

    /** @brief Scene-tree name of a chunk object, e.g. `chunk_3_-2`. */
    static std::string chunk_object_name_(const ChunkCoord& coord) {
        return "chunk_" + std::to_string(coord.x) + "_" + std::to_string(coord.y);
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;
    /// The shared baked surface-detail texture (terrain_detail_texture.h); baked on the first
    /// frame with terrain and handed to every chunk material.
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> detail_texture_;

    /// @brief Scratch list of queued coordinates, reused across frames so sorting allocates once.
    std::vector<ChunkCoord> pending_;
    /// @brief The same, for the finished chunks waiting to be published this frame.
    std::vector<ChunkCoord> finished_;

    /// Frames a retired chunk's collision mesh is kept alive (see the file doc).
    static constexpr std::uint64_t k_collision_keep_frames = 600;

    std::uint64_t frame_ = 0;          /**< execute() calls, for retiring collision meshes. */
    /** @brief Chunks meshed and published since this system was installed -- for the churn a
     *         streaming test measures, and for the debug overlay. */
    std::uint64_t chunks_built_ = 0;
    /** @brief Chunks destroyed outright (a parked one that is revived counts for neither). */
    std::uint64_t chunks_released_ = 0;
    double        last_upload_ms_ = 0.0; /**< upload_finished_chunks_()'s own cost (diagnostics). */
    bool          physics_dirty_ = false; /**< A chunk collider came or went this frame. */
    /** @brief Each planet terrain's gravity source in the physics world (sync_planet_gravity_()). */
    std::unordered_map<const TerrainComponent*, std::uint32_t> planet_gravity_;
    /// Planets whose every chunk has been live: their pull is on (and stays on through edits).
    std::unordered_set<const TerrainComponent*> planet_ready_;
};

/**
 * @brief Constructs a TerrainSystem and registers it ahead of the physics phase.
 *
 * Mirrors toy::scene::install_kinematic_control_system(). Call once per scene, before the first
 * update. The captured references must outlive the scene.
 *
 * @param scene     Scene to install into.
 * @param device    Vulkan logical device.
 * @param allocator VMA allocator.
 * @param assets    Asset manager the chunk meshes are published into.
 * @param order     Registration order; defaults to k_terrain_system_order (60).
 * @return Non-owning pointer to the installed system.
 */
TerrainSystem* install_terrain_system(coopa::scene::Scene& scene,
                                             coopa::gfx::core::Device& device,
                                             coopa::gfx::memory::Allocator& allocator,
                                             coopa::asset::AssetManager& assets,
                                             int order = k_terrain_system_order);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_SYSTEM_H
