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
 * Off-thread (any worker): sampling the map and merging sides into vertex/index buffers. That is
 * nearly all the cost, it is pure (see terrain_chunk.h), and chunks are independent.
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
 *     before it is actually destroyed. That is the same mechanism a hot reload uses, and it is
 *     why this file does no frame counting of its own.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_SYSTEM_H
#define TOYENGINE_WORLD_TERRAIN_SYSTEM_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>

#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_types.h>

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

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        for (TerrainComponent* terrain : scene.get_components<TerrainComponent>()) {
            tick_terrain_(*terrain, scene, ctx);
        }
    }

private:
    /**
     * @brief Follows AssetManager::k_asset_io_job_type's precedent: any value >= k_max_job_types
     *        (64) opts out of thread dedication and diagnostics counters, which is what a batch
     *        of independent, dependency-free jobs wants.
     */
    static constexpr coopa::job::JobType k_terrain_job_type = 0x7E44A100u;

    /** @brief One terrain's whole frame: generate, then stream. */
    void tick_terrain_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                       const coopa::scene::FrameContext& ctx) {
        terrain.begin_generation(ctx.jobs);
        terrain.poll_generation();
        terrain.poll_side_meshes();

        // Retiring chunks are advanced even before the terrain is ready: a scene torn down
        // mid-generation still has handles to close.
        advance_retiring_(terrain);
        if (!terrain.is_ready() || ctx.jobs == nullptr) return;

        const ChunkCoord centre = terrain.params.chunk_at(camera_position_(terrain));
        want_chunks_(terrain, centre);
        release_distant_chunks_(terrain, centre, ctx);
        upload_finished_chunks_(terrain, scene);
        submit_queued_chunks_(terrain, centre, ctx);
    }

    /**
     * @brief The position the loaded region is centred on.
     *
     * The main camera when there is one, falling back to the terrain object's own origin -- so a
     * headless test, or a scene authored before its camera exists, still builds the chunk under
     * the terrain rather than nothing at all.
     *
     * get_world_matrix(), not world_matrix(): this system runs at order 60, BEFORE
     * UpdatePhase::TransformResolve (350), so on the first frame no resolve pass has run yet and
     * the non-resolving read would trip its own debug assert. Resolving here is safe because
     * this is the owner thread and nothing else is reading concurrently -- the restriction on
     * get_world_matrix() is against concurrent readers, not against calling it at all.
     */
    glm::vec3 camera_position_(const TerrainComponent& terrain) const {
        if (CameraComponent* camera = CameraComponent::main()) {
            if (camera->owner != nullptr) {
                if (auto* tc = camera->owner->get_transform()) {
                    return glm::vec3(tc->transform().get_world_matrix()[3]);
                }
            }
        }
        if (terrain.owner != nullptr) {
            if (auto* tc = terrain.owner->get_transform()) {
                return glm::vec3(tc->transform().get_world_matrix()[3]);
            }
        }
        return glm::vec3(0.0f);
    }

    /** @brief True when any part of this chunk lies inside the generated map. */
    static bool chunk_in_world(const TerrainComponent& terrain, const ChunkCoord& coord) {
        const std::int32_t tiles = terrain.sampler().tiles_per_axis();
        const std::int32_t size  = terrain.params.chunk_size;
        return coord.x >= 0 && coord.y >= 0 && coord.x * size < tiles && coord.y * size < tiles;
    }

    /** @brief Inserts a Queued entry for every in-range chunk that does not exist yet. */
    void want_chunks_(TerrainComponent& terrain, const ChunkCoord& centre) {
        const std::int32_t radius = terrain.params.view_radius;
        for (std::int32_t dy = -radius; dy <= radius; ++dy) {
            for (std::int32_t dx = -radius; dx <= radius; ++dx) {
                const ChunkCoord coord{centre.x + dx, centre.y + dy};
                if (!chunk_in_world(terrain, coord)) continue;
                if (terrain.chunks().find(coord) != terrain.chunks().end()) continue;

                TerrainChunk chunk;
                chunk.coord = coord;
                chunk.state = ChunkState::Queued;
                terrain.chunks().emplace(coord, std::move(chunk));
            }
        }
    }

    /**
     * @brief Drops chunks that have left the loaded region.
     *
     * The threshold is `view_radius + 1`, not `view_radius`: a camera sitting exactly on a chunk
     * boundary otherwise oscillates a whole ring of chunks between built and dropped every time
     * it drifts a few centimetres, which is the most expensive thing this system can do.
     */
    void release_distant_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                                 const coopa::scene::FrameContext& ctx) {
        const std::int32_t keep = terrain.params.view_radius + 1;
        for (auto it = terrain.chunks().begin(); it != terrain.chunks().end();) {
            TerrainChunk& chunk = it->second;
            if (chunk.state == ChunkState::Retiring || chunk_distance(chunk.coord, centre) <= keep) {
                ++it;
                continue;
            }

            if (chunk.state == ChunkState::Meshing) {
                // Cancel only stops jobs that have not started; either way the handle has to
                // survive until the counter drains, so park it rather than erasing here.
                ctx.jobs->cancel(chunk.job);
                chunk.state = ChunkState::Retiring;
                chunk.build.reset(); // the job holds its own reference; this one is dead weight
                ++it;
                continue;
            }

            destroy_chunk_object_(terrain, chunk);
            it = terrain.chunks().erase(it);
        }
    }

    /** @brief Closes and erases retiring chunks whose in-flight job has finally stopped. */
    void advance_retiring_(TerrainComponent& terrain) {
        for (auto it = terrain.chunks().begin(); it != terrain.chunks().end();) {
            TerrainChunk& chunk = it->second;
            if (chunk.state != ChunkState::Retiring) {
                ++it;
                continue;
            }
            if (!chunk.job.is_complete()) {
                ++it;
                continue;
            }
            chunk.job.close();
            chunk.job = coopa::job::JobHandle{}; // same one-close rule as upload_finished_chunks_
            destroy_chunk_object_(terrain, chunk);
            it = terrain.chunks().erase(it);
        }
    }

    /**
     * @brief Uploads every chunk whose meshing job has finished, up to this frame's budget.
     *
     * The budget applies here as well as to submission because this is the half that runs on the
     * main thread: a teleport that queues fifty chunks should spend fifty small uploads over
     * several frames, not one long stall.
     */
    void upload_finished_chunks_(TerrainComponent& terrain, coopa::scene::Scene& scene) {
        int budget = std::max(1, terrain.max_chunk_jobs_per_frame);
        for (auto& entry : terrain.chunks()) {
            if (budget <= 0) break;
            TerrainChunk& chunk = entry.second;
            if (chunk.state != ChunkState::Meshing || !chunk.job.is_complete()) continue;

            chunk.job.close();
            // Cleared, not merely closed: a closed handle's pool slot is recycled, so leaving
            // the stale token in place would let ~TerrainComponent()'s drain close it a second
            // time -- which the CounterPool asserts on, by design.
            chunk.job = coopa::job::JobHandle{};
            chunk.state = ChunkState::Live;
            --budget;

            // An empty chunk is a real outcome, not a failure: a chunk of open ocean below the
            // waterline emits no faces at all. It stays Live with no object, so it is never
            // re-queued and never draws.
            if (chunk.build && !chunk.build->mesh.empty()) {
                create_chunk_object_(terrain, scene, chunk);
            }
            chunk.build.reset();
        }
    }

    /** @brief Submits meshing jobs for the nearest queued chunks, up to this frame's budget. */
    void submit_queued_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                               const coopa::scene::FrameContext& ctx) {
        const int budget = std::max(1, terrain.max_chunk_jobs_per_frame);

        pending_.clear();
        for (auto& entry : terrain.chunks()) {
            if (entry.second.state == ChunkState::Queued) pending_.push_back(entry.first);
        }
        if (pending_.empty()) return;

        // Nearest first, so the ground under the camera appears before the horizon does. The tie
        // break on coordinates keeps the order deterministic for a given camera position, which
        // is what makes a scripted capture reproducible.
        std::sort(pending_.begin(), pending_.end(),
                  [&centre](const ChunkCoord& a, const ChunkCoord& b) {
                      const std::int32_t da = chunk_distance(a, centre);
                      const std::int32_t db = chunk_distance(b, centre);
                      if (da != db) return da < db;
                      if (a.y != b.y) return a.y < b.y;
                      return a.x < b.x;
                  });

        const int submitted_count = std::min<int>(budget, static_cast<int>(pending_.size()));
        for (int i = 0; i < submitted_count; ++i) {
            TerrainChunk& chunk = terrain.chunks().at(pending_[static_cast<std::size_t>(i)]);

            auto build = std::make_shared<ChunkBuildJob>();
            build->sampler = &terrain.sampler();
            build->library = &terrain.library();
            build->params  = terrain.params;
            build->coord   = chunk.coord;

            chunk.build = build;
            chunk.job   = ctx.jobs->create_handle();
            chunk.state = ChunkState::Meshing;

            // One job per chunk rather than one parallel_for over all of them: chunks finish and
            // appear individually, and a chunk that leaves view mid-flight can be cancelled on
            // its own without disturbing its neighbours.
            ctx.jobs->submit([build]() { build->run(); }, k_terrain_job_type, chunk.job, nullptr,
                             0u, coopa::job::Priority::Normal);
        }
    }

    /** @brief Creates the GPU mesh, publishes it, and hangs a drawable child off the terrain. */
    void create_chunk_object_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                              TerrainChunk& chunk) {
        if (terrain.owner == nullptr) return;

        const ChunkMeshData& data = chunk.build->mesh;
        // buffer_count 1: a chunk mesh is written once and never rewritten, unlike a cloth's
        // per-frame ring (see Mesh::from_arrays()'s doc on why that is a count, not a bool).
        chunk.mesh = std::make_shared<Mesh>(
            Mesh::from_arrays(device_, allocator_, data.vertices, data.indices, 1));

        const std::string id = chunk_asset_id_(terrain, chunk.coord);
        auto handle = assets_.create<Mesh>(id, chunk.mesh);

        auto object = std::make_unique<coopa::scene::SceneObject>(chunk_object_name_(chunk.coord));
        auto* transform = object->add_component<TransformComponent>();
        transform->transform().set_position(chunk_origin(terrain.params, chunk.coord));

        auto* renderer = object->add_component<MeshRenderer>();
        renderer->material = terrain.material;
        renderer->set_mesh(std::move(handle));

        chunk.object = terrain.owner->add_child(std::move(object));
        // Stamps the Scene back-pointer onto the new components, exactly as
        // Scene::flush_commands() does for a deferred add -- see Scene::adopt().
        scene.adopt(*chunk.object);
    }

    /** @brief Destroys a chunk's SceneObject and retires its published mesh. */
    void destroy_chunk_object_(TerrainComponent& terrain, TerrainChunk& chunk) {
        if (chunk.object != nullptr && terrain.owner != nullptr) {
            // The returned unique_ptr is destroyed immediately, which destroys the MeshRenderer
            // and with it the last AssetHandle on the chunk's mesh -- the refcount unload()
            // requires to have reached zero.
            terrain.owner->detach_child(chunk.object);
            chunk.object = nullptr;
        }
        if (chunk.mesh) {
            chunk.mesh.reset();
            assets_.unload(coopa::asset::AssetId::from_path(chunk_asset_id_(terrain, chunk.coord)));
        }
    }

    /** @brief Synthetic asset id for a chunk mesh, prefixed to stay out of the real-path namespace. */
    static std::string chunk_asset_id_(const TerrainComponent& terrain, const ChunkCoord& coord) {
        const std::string owner = terrain.owner != nullptr ? terrain.owner->name() : "terrain";
        return "runtime/terrain/" + owner + "/" + std::to_string(coord.x) + "_" +
               std::to_string(coord.y);
    }

    /** @brief Scene-tree name of a chunk object, e.g. `chunk_3_-2`. */
    static std::string chunk_object_name_(const ChunkCoord& coord) {
        return "chunk_" + std::to_string(coord.x) + "_" + std::to_string(coord.y);
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;

    /// @brief Scratch list of queued coordinates, reused across frames so sorting allocates once.
    std::vector<ChunkCoord> pending_;
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
inline TerrainSystem* install_terrain_system(coopa::scene::Scene& scene,
                                             coopa::gfx::core::Device& device,
                                             coopa::gfx::memory::Allocator& allocator,
                                             coopa::asset::AssetManager& assets,
                                             int order = k_terrain_system_order) {
    auto system = std::make_unique<TerrainSystem>(device, allocator, assets);
    TerrainSystem* raw = system.get();
    scene.add_system(std::move(system), order);
    return raw;
}

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_SYSTEM_H
