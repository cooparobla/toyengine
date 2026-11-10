/**
 * @file routine_system.h
 * @brief The Routine-phase ISceneSystem, and the Component-facing helpers that
 *        make starting a routine read like Unity's `StartCoroutine`.
 *
 * This is the only header in the module that knows about coopa/scene, matching
 * the direction coopa::anim::AnimationSystem already establishes: the scene
 * module never includes anything from here.
 */

#ifndef COOPA_ROUTINE_ROUTINE_SYSTEM_H
#define COOPA_ROUTINE_ROUTINE_SYSTEM_H

#include <cstddef>
#include <utility>

#include <coopa/routine/routine.h>
#include <coopa/routine/runner.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

namespace coopa {
namespace routine {

/**
 * @class RoutineSystem
 * @brief Registered at coopa::scene::UpdatePhase::Routine; resumes every
 *        running Routine once per frame.
 *
 * Landing at 250 puts routine resumption after the built-in
 * `Component::update()` walk and before `Component::late_update()`, which is
 * where Unity resumes coroutines in its frame. Not auto-installed -- register
 * it the same way AnimationSystem is registered:
 *
 * @code
 * scene.add_system(std::make_unique<coopa::routine::RoutineSystem>(),
 *                  coopa::scene::UpdatePhase::Routine);
 * @endcode
 *
 * Routine bodies resume inline on the thread driving `Scene::update()`, so
 * they may mutate the Scene as freely as `Component::update()` may. The
 * Scene's JobEngine is forwarded to the runner purely so `on_worker()` has
 * somewhere to submit to.
 */
class RoutineSystem : public coopa::scene::ISceneSystem {
public:
    /// @brief The name Scene::find_system() and remove_system() match on.
    const char* system_name() const override { return "Routine"; }

    /// @brief Stops every routine still running when this system leaves the Scene.
    void on_detach(coopa::scene::Scene& scene) override {
        (void)scene;
        runner_.stop_all();
    }

    /**
     * @brief Pumps every running routine once.
     * @param scene The scene this system belongs to.
     * @param ctx This frame's inputs; supplies the delta and the JobEngine.
     */
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        (void)scene;
        runner_.set_job_engine(ctx.jobs);
        runner_.tick(ctx.delta_time * time_scale_, ctx.delta_time);
    }

    /**
     * @brief The runner this system pumps.
     * @return Reference to the owned RoutineRunner.
     */
    RoutineRunner& runner() { return runner_; }

    /// @brief Const overload of runner().
    const RoutineRunner& runner() const { return runner_; }

    /**
     * @brief Multiplier applied to the frame delta before it reaches `seconds()`.
     *
     * Unity's `Time.timeScale`. Setting it to 0 pauses every `seconds()` wait
     * while leaving `seconds_realtime()` running at wall-clock speed, which is
     * exactly what a pause menu needs.
     *
     * @param scale The new time scale. Negative values are clamped to zero.
     */
    void set_time_scale(float scale) { time_scale_ = scale > 0.0f ? scale : 0.0f; }

    /**
     * @brief The current time scale.
     * @return The multiplier applied to the frame delta.
     */
    float time_scale() const { return time_scale_; }

private:
    RoutineRunner runner_;     /**< Owns every routine started through this scene. */
    float time_scale_ = 1.0f;  /**< See set_time_scale(). */
};

/**
 * @brief Finds the RoutineRunner of the RoutineSystem registered on a scene.
 * @param scene The scene to search.
 * @return The runner, or nullptr if no RoutineSystem is registered.
 */
inline RoutineRunner* runner_for(coopa::scene::Scene& scene) {
    auto* system = dynamic_cast<RoutineSystem*>(scene.find_system("Routine"));
    return system ? &system->runner() : nullptr;
}

/**
 * @brief Finds the RoutineRunner reachable from a component's scene.
 * @param component The component to start from.
 * @return The runner, or nullptr if the component has no scene yet or that
 *         scene has no RoutineSystem registered.
 */
inline RoutineRunner* runner_for(coopa::scene::Component& component) {
    return component.scene ? runner_for(*component.scene) : nullptr;
}

/**
 * @brief Starts a routine owned by a component -- the `StartCoroutine` equivalent.
 *
 * The component's address becomes the routine's owner token, so
 * stop_routines(component) stops exactly the routines it started. To have that
 * happen automatically when the component dies, give it a RoutineScope member
 * and call `scope.start(*this, ...)` instead.
 *
 * @param component The component the routine belongs to.
 * @param r The routine to run.
 * @return A handle naming the routine, or an empty handle if no RoutineSystem
 *         is registered on the component's scene.
 */
inline RoutineHandle start_routine(coopa::scene::Component& component, Routine&& r) {
    RoutineRunner* runner = runner_for(component);
    return runner ? runner->start(std::move(r), &component) : RoutineHandle();
}

/**
 * @brief Stops every routine started for a component -- `StopAllCoroutines`.
 * @param component The component whose routines to stop.
 * @return How many running routines were stopped.
 */
inline size_t stop_routines(coopa::scene::Component& component) {
    RoutineRunner* runner = runner_for(component);
    return runner ? runner->stop_owner(&component) : 0;
}

inline RoutineHandle RoutineScope::start(coopa::scene::Component& owner, Routine&& r) {
    RoutineRunner* runner = runner_for(owner);
    if (!runner) return RoutineHandle();
    return start(*runner, std::move(r), &owner);
}

} // namespace routine
} // namespace coopa

#endif // COOPA_ROUTINE_ROUTINE_SYSTEM_H
