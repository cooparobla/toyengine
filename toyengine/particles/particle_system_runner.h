/**
 * @file particle_system_runner.h
 * @brief `ParticleSimulationSystem` -- steps every live ParticleSystem once per frame on the
 *        scene's JobEngine, forwards sub-emitter deaths, and (for Engine) gathers the frame's
 *        draw batches.
 *
 * Order **360**: after TransformResolve (350), so every emitter's world matrix is current when
 * it is read, and still inside Scene::update() (< LateBehaviour, 400), so a system that a
 * script stops or moves in late_update() is seen next frame like any other component. Runs in
 * edit mode too: effects preview live in the editor's viewport, the way Blender plays particles
 * in the viewport and Unity previews a selected system.
 *
 * ## Threading
 *
 * Two levels, both over the frame's JobEngine:
 * - **Across systems.** Systems below `k_parallel_particles` live particles are stepped as
 *   independent jobs (one system per index) -- a scene full of torches costs one dispatch.
 * - **Within a system.** A large system steps on the calling thread, splitting its own
 *   per-particle update, bounds and render prep across the workers.
 *
 * World matrices are read serially first (get_world_matrix() may resolve lazily), and spawning
 * inside one system is serial, so every result is identical on any worker count.
 */

#ifndef TOYENGINE_PARTICLES_PARTICLE_SYSTEM_RUNNER_H
#define TOYENGINE_PARTICLES_PARTICLE_SYSTEM_RUNNER_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <unordered_set>
#include <vector>

#include <coopa/job/engine.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/particles/particle_system.h>
#include <toyengine/render/particle_types.h>

namespace toy {
namespace particles {

inline constexpr int k_particle_system_order = 360;

/// Below this many live particles a system is stepped whole as one job; at or above it, its own
/// update is split across the workers instead.
inline constexpr std::size_t k_parallel_particles = 2048;

class ParticleSimulationSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "Particles"; }
    bool runs_in_edit_mode() const override { return true; }

    /**
     * @brief GPU simulation (`simulation: gpu` systems): `enabled` is particles.gpu_enabled and
     *        the device's compute support; `alive` reads the renderer's alive-count readback
     *        (id -> count, generation). Off (the default): every system simulates on the CPU.
     */
    void set_gpu(bool enabled, std::function<bool(uint64_t, uint32_t&, uint32_t&)> alive);
    bool gpu_enabled() const { return gpu_enabled_; }

    /** @brief Forces inline execution (tests compare it against the parallel path). */
    void set_force_serial(bool serial) { force_serial_ = serial; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /** @brief One frame for every active system in `scene`. Public so tools can drive it. */
    void step(coopa::scene::Scene& scene, float dt);

    /**
     * @brief Fills `out` with this frame's draw batches for every system visible from the view
     *        (`cull` tests a world AABB; null draws everything), preparing instances for those
     *        only -- in parallel across systems, and within a large one.
     */
    template <typename CullFn>
    void collect_render(coopa::scene::Scene& scene, const ParticleView& view, const CullFn& cull,
                        render::ParticleFrameState& out) {
        out.quads.clear();
        out.meshes.clear();
        out.gpu.clear();
        gather_(scene);
        visible_.clear();
        for (ParticleSystem* ps : systems_) {
            const auto& s = ps->settings;
            if (ps->gpu_active()) {
                // Simulated every frame, seen or not; drawn (indirectly) only when visible.
                render::GpuParticleJob job;
                if (!ps->make_gpu_job(view, job)) continue;
                out.gpu.push_back(std::move(job));
                if (s.look.mode == render::ParticleRenderMode::None) continue;
                if (s.max_draw_distance > 0.0f &&
                    glm::distance(view.camera_pos, ps->emitter_position()) > s.max_draw_distance) continue;
                if (!cull(ps->bounds_min(), ps->bounds_max())) continue;
                render::ParticleDrawBatch b;
                b.gpu_id = ps->gpu_id();
                b.count = std::max(1u, ps->particle_count());
                b.sort_center = ps->sort_center();
                b.bounds_min = ps->bounds_min();
                b.bounds_max = ps->bounds_max();
                b.look = s.look;
                b.texture_material = ps->has_texture() ? &ps->texture_material : nullptr;
                if (!b.texture_material && b.look.sprite == render::ParticleSprite::Texture) b.look.sprite = render::ParticleSprite::Soft;
                out.quads.push_back(b);
                continue;
            }
            if (ps->particle_count() == 0 || s.look.mode == render::ParticleRenderMode::None) continue;
            if (s.max_draw_distance > 0.0f &&
                glm::distance(view.camera_pos, ps->emitter_position()) > s.max_draw_distance) continue;
            if (!cull(ps->bounds_min(), ps->bounds_max())) continue;
            if (s.look.mode == render::ParticleRenderMode::Mesh &&
                (!ps->has_mesh_proxy() || !ps->mesh_proxy().is_ready())) continue;
            visible_.push_back(ps);
        }

        const ParallelFor par = parallel_(512);
        small_.clear();
        for (std::size_t i = 0; i < visible_.size(); ++i) {
            if (visible_[i]->particle_count() >= k_parallel_particles) visible_[i]->prepare_render(view, &par);
            else small_.push_back(i);
        }
        const ParallelFor across = parallel_(2);
        run_range(&across, small_.size(), [&](std::size_t b, std::size_t e) {
            for (std::size_t k = b; k < e; ++k) visible_[small_[k]]->prepare_render(view, nullptr);
        });

        for (ParticleSystem* ps : visible_) {
            if (ps->settings.look.mode == render::ParticleRenderMode::Mesh) {
                render::ParticleMeshBatch mb;
                mb.proxy = &ps->mesh_proxy();
                mb.matrices = ps->instance_matrices().data();
                mb.count = static_cast<uint32_t>(ps->instance_matrices().size());
                mb.bounds_min = ps->bounds_min();
                mb.bounds_max = ps->bounds_max();
                if (mb.count) out.meshes.push_back(mb);
                continue;
            }
            render::ParticleDrawBatch b;
            b.instances = ps->instances().data();
            b.count = static_cast<uint32_t>(ps->instances().size());
            b.sort_center = ps->sort_center();
            b.bounds_min = ps->bounds_min();
            b.bounds_max = ps->bounds_max();
            b.look = ps->settings.look;
            b.texture_material = ps->has_texture() ? &ps->texture_material : nullptr;
            if (!b.texture_material && b.look.sprite == render::ParticleSprite::Texture) b.look.sprite = render::ParticleSprite::Soft;
            if (b.count) out.quads.push_back(b);
        }
    }

    const std::vector<ParticleSystem*>& systems() const { return systems_; }

    /** @brief Live particles across every system after the last step (for stats/tests). */
    std::size_t total_particles() const;

private:
    /** @brief Which systems simulate on the GPU this step (one warning per system that asked
     *         for it and cannot), and their alive counts fed back from the renderer. */
    void assign_gpu_();

    void gather_(coopa::scene::Scene& scene);

    static bool active_in_hierarchy_(const coopa::scene::SceneObject& obj);

    void resolve_sub_targets_(coopa::scene::Scene& scene, ParticleSystem& ps);

    ParallelFor parallel_(std::size_t min_items) const;

    coopa::job::JobEngine* jobs_ = nullptr;
    bool force_serial_ = false;
    bool gpu_enabled_ = false;
    std::function<bool(uint64_t, uint32_t&, uint32_t&)> gpu_alive_;
    std::unordered_set<const ParticleSystem*> sub_targets_;
    std::vector<ParticleSystem*> systems_;
    std::vector<ParticleSystem*> visible_;
    std::vector<glm::mat4> worlds_;
    std::vector<std::size_t> small_;
};

/** @brief Installs the system on `scene` (once; later calls return the existing one). */
ParticleSimulationSystem* install_particle_system(coopa::scene::Scene& scene,
                                                         int order = k_particle_system_order);

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_PARTICLE_SYSTEM_RUNNER_H
