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
 * Off-thread (any worker): assembling tile-set pieces into vertex/index buffers and building the
 * chunk's collision mesh (weld, adjacency, BVH). The chunk's columns are copied out of the
 * terrain's tiles on the owner thread when the job is submitted (a plain copy), so the tiles can
 * be edited at any time; an edit marks the chunks it touches stale, and each rebuilds while its
 * old mesh keeps drawing, then swaps in. That is nearly all
 * the cost, it is pure (see terrain_chunk.h), and chunks are independent.
 *
 * Owner thread only: creating GPU buffers, publishing assets, and touching the scene tree.
 * `Mesh::from_arrays()` allocates Vulkan memory, `AssetManager` is single-owner, and a Scene is
 * single-owner -- none of the three may be reached from a job.
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

private:
    /**
     * @brief Follows AssetManager::k_asset_io_job_type's precedent: any value >= k_max_job_types
     *        (64) opts out of thread dedication and diagnostics counters, which is what a batch
     *        of independent, dependency-free jobs wants.
     */
    static constexpr coopa::job::JobType k_terrain_job_type = 0x7E44A100u;

    /** @brief One terrain's whole frame: generate, then stream. */
    void tick_terrain_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                       const coopa::scene::FrameContext& ctx);

    /**
     * @brief The point the loaded region is centred on, in the terrain's local space: where the
     *        main camera's view ray meets the tiles (or the terrain's base plane) within a few
     *        view radii, else the camera's own position -- so an orbit or editor camera viewing
     *        the terrain from afar still loads what it looks at. The terrain's origin without a
     *        camera, so a headless test still builds the chunk under it.
     *
     * get_world_matrix(), not world_matrix(): this system runs at order 60, BEFORE
     * UpdatePhase::TransformResolve (350), so on the first frame no resolve pass has run yet and
     * the non-resolving read would trip its own debug assert. Resolving here is safe because
     * this is the owner thread and nothing else is reading concurrently.
     */
    glm::vec3 stream_centre_(const TerrainComponent& terrain) const;

    /** @brief True when any part of this chunk lies inside the generated map. */
    static bool chunk_in_world(const TerrainComponent& terrain, const ChunkCoord& coord);

    /** @brief Inserts a Queued entry for every in-range chunk that does not exist yet. */
    void want_chunks_(TerrainComponent& terrain, const ChunkCoord& centre);

    /**
     * @brief Drops chunks that have left the loaded region.
     *
     * The threshold is `view_radius + 1`, not `view_radius`: a camera sitting exactly on a chunk
     * boundary otherwise oscillates a whole ring of chunks between built and dropped every time
     * it drifts a few centimetres, which is the most expensive thing this system can do.
     */
    void release_distant_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                                 const coopa::scene::FrameContext& ctx);

    /** @brief Closes and erases retiring chunks whose in-flight job has finally stopped. */
    void advance_retiring_(TerrainComponent& terrain);

    /**
     * @brief Uploads every chunk whose meshing job has finished, up to this frame's budget.
     *
     * The budget applies here as well as to submission because this is the half that runs on the
     * main thread: a teleport that queues fifty chunks should spend fifty small uploads over
     * several frames, not one long stall.
     */
    void upload_finished_chunks_(TerrainComponent& terrain, coopa::scene::Scene& scene);

    /** @brief Submits meshing jobs for the nearest queued chunks, up to this frame's budget. */
    void submit_queued_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                               const coopa::scene::FrameContext& ctx);

    /** @brief Creates the GPU mesh, publishes it, and hangs a drawable (and collidable) child off the terrain. */
    void create_chunk_object_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                              TerrainChunk& chunk);

    /** @brief Destroys a chunk's SceneObject and retires its published mesh. */
    void destroy_chunk_object_(TerrainComponent& terrain, TerrainChunk& chunk);

    /** @brief Synthetic asset id for a chunk's current mesh, prefixed to stay out of the real-path namespace. */
    static std::string chunk_asset_id_(const TerrainComponent& terrain, const TerrainChunk& chunk);

    /**
     * @brief Marks every chunk whose tiles (or skirt) were edited since the last frame stale, so
     *        submit_queued_chunks_() rebuilds it -- nearest first, like new chunks.
     */
    void mark_edited_chunks_(TerrainComponent& terrain);

    /** @brief Scene-tree name of a chunk object, e.g. `chunk_3_-2`. */
    static std::string chunk_object_name_(const ChunkCoord& coord) {
        return "chunk_" + std::to_string(coord.x) + "_" + std::to_string(coord.y);
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;

    /// @brief Scratch list of queued coordinates, reused across frames so sorting allocates once.
    std::vector<ChunkCoord> pending_;

    /// Frames a retired chunk's collision mesh is kept alive (see the file doc).
    static constexpr std::uint64_t k_collision_keep_frames = 600;

    std::uint64_t frame_ = 0;          /**< execute() calls, for retiring collision meshes. */
    bool          physics_dirty_ = false; /**< A chunk collider came or went this frame. */
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
