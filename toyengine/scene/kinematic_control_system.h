/**
 * @file kinematic_control_system.h
 * @brief Runs every component that drives a KINEMATIC body's Transform, ahead of the physics step.
 *
 * This exists to fix an ordering bug, not as a stylistic preference, so the reasoning is worth
 * stating plainly.
 *
 * A kinematic body is authoritative over its own pose: `PhysicsSystem::sync_transforms_in_()` hard-
 * sets `Body::position` from the owner's Transform each frame and derives the body's velocity from
 * the frame-to-frame delta. But `PhysicsSystem` runs at `UpdatePhase::Physics` (100), while an
 * ordinary component's `update()` runs at `Behaviour` (200) -- so a component that writes that
 * Transform in `update()` is always one frame too late:
 *
 * @code
 *   100 Physics     sync_transforms_in_ reads the Transform  P(f-1)   <- physics solves against this
 *   200 Behaviour   KinematicController writes               P(f)
 *   350 TransformResolve -> render:  drawn at P(f)
 * @endcode
 *
 * Everything physics computed that frame -- contacts, and in particular a cloth draped over the
 * body -- is therefore positioned around P(f-1), while the renderer draws the body at P(f). For a
 * rigid contact that is invisible. For cloth it is not: the measured mismatch on
 * `assets/scenes/cloth_test` at `move_speed: 4.0` is ~1 cm, against a 3 cm collision standoff, and
 * shows up as the ball poking through the sheet on its leading side.
 *
 * Running these components at order 50 -- ahead of Physics (100), and still inside `Scene::update()`
 * since 50 < `LateBehaviour` (400) -- closes the gap exactly: physics reads the pose that will be
 * drawn. `Scene::add_system(std::unique_ptr, int order)` documents this inter-phase insertion as a
 * supported use.
 *
 * The components keep their logic; they just expose it as `advance(dt)` instead of `update(dt)`, so
 * the Behaviour walk no longer double-applies it. A scene that never installs this system simply
 * does not move them at all, which is a loud failure rather than a subtly wrong one.
 */

#ifndef TOYENGINE_SCENE_KINEMATIC_CONTROL_SYSTEM_H
#define TOYENGINE_SCENE_KINEMATIC_CONTROL_SYSTEM_H

#include <memory>
#include <vector>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/kinematic_mover.h>

namespace toy {
namespace scene {

/** @brief Default registration order: ahead of `UpdatePhase::Physics` (100), still inside update(). */
inline constexpr int k_kinematic_control_order = 50;

/**
 * @class KinematicControlSystem
 * @brief Advances every KinematicController and KinematicMover in the scene, before physics reads
 *        their Transforms.
 */
class KinematicControlSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "KinematicControl"; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        // Deliberately serial. These components write Transforms, and a Transform write dirties its
        // whole child subtree -- two controllers on objects in the same subtree would race on that
        // flag. There are never many of them, and the work is a handful of flops each.
        for (KinematicController* kc : scene.get_components<KinematicController>()) {
            kc->advance(ctx.delta_time);
        }
        for (KinematicMover* km : scene.get_components<KinematicMover>()) {
            km->advance(ctx.delta_time);
        }
    }
};

/**
 * @brief Constructs a KinematicControlSystem and registers it ahead of the physics phase.
 *
 * Mirrors `coopa::scene::install_transform_system()`. Call once per scene, before the first
 * update; order relative to `install_physics_system()` does not matter, only the numeric order does.
 *
 * @param scene Scene to install into.
 * @param order Registration order; defaults to k_kinematic_control_order (50).
 * @return Non-owning pointer to the installed system.
 */
inline KinematicControlSystem* install_kinematic_control_system(
    coopa::scene::Scene& scene, int order = k_kinematic_control_order) {
    auto sys = std::make_unique<KinematicControlSystem>();
    KinematicControlSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_KINEMATIC_CONTROL_SYSTEM_H
