/**
 * @file scene_system.h
 * @brief Ordered per-frame system pipeline that Scene::update()/late_update() drive.
 *
 * A Scene runs a small ordered list of ISceneSystem instances, of which the
 * recursive Component::update()/late_update() walks are just the two built-in
 * members (BehaviourSystem and LateBehaviourSystem). This gives every
 * subsystem (animation, transform resolution, a future physics system) a
 * defined point in the frame to run at, and a single place to reach the
 * Scene's JobEngine — see FrameContext::jobs below.
 */

#ifndef COOPA_SCENE_SCENE_SYSTEM_H
#define COOPA_SCENE_SCENE_SYSTEM_H

#include <cstdint>

// Forward declaration only — scene_system.h (and therefore scene.h) must not
// include coopa/job/engine.h. coopa/scene/README.md restricts the module to
// vendored fkYAML/glm plus libcoopa's own coopa/event; a raw JobEngine* here
// keeps that true and keeps engine.h's <thread>/<condition_variable>/
// <iostream> out of every scene.h translation unit. A system that actually
// submits jobs includes coopa/job/engine.h itself.
namespace coopa {
namespace job {
class JobEngine;
} // namespace job
} // namespace coopa

namespace coopa {
namespace scene {

class Scene;
class SceneCommandBuffer; // Forward declaration only, for the same reason as JobEngine above.

/**
 * @brief Ordering slots for scene systems, spaced by 100 so a consumer can
 *        insert a new system between built-ins without renumbering anything.
 *
 * Scene::update() runs every system with order < LateBehaviour;
 * Scene::late_update() runs every system with order >= LateBehaviour. That
 * split keeps each public entry point responsible for exactly the phases named
 * after it, and lands Routine and Animation before the built-in late_update()
 * walk, matching Unity's Update -> coroutines -> animation -> LateUpdate order.
 */
enum class UpdatePhase : int {
    Physics         = 100, ///< Reserved. No implementation ships in libcoopa.
    Behaviour       = 200, ///< Built-in: the recursive Component::update() walk.
    Routine         = 250, ///< coopa::routine::RoutineSystem: resume every running Routine.
    Animation       = 300, ///< coopa::anim::AnimationSystem: evaluate + apply.
    TransformResolve = 350, ///< coopa::scene::TransformSystem: top-down world-matrix resolve.
    LateBehaviour   = 400, ///< Built-in: the recursive Component::late_update() walk.
};

/**
 * @struct FrameContext
 * @brief Per-frame inputs handed to every ISceneSystem::execute() call.
 */
struct FrameContext {
    float    delta_time  = 0.0f; /**< Seconds since the last frame. */
    uint64_t frame_index = 0;    /**< Monotonically increasing frame counter, from Scene::frame_index(). */

    /**
     * @brief The Scene's JobEngine, or nullptr.
     *
     * nullptr means no engine was ever installed via Scene::set_job_engine()
     * — every system must then do its work inline on the calling thread. This
     * is the default, so an application that never touches set_job_engine()
     * spawns no threads and every system runs inline on the calling thread.
     *
     * A system MAY call create_handle()/submit()/submit_jobs()/parallel_for()/
     * wait_for() on this engine, and MAY call begin_frame()/end_frame() on it
     * (those are diagnostics-only -- see coopa/job/handle.h -- so there is no
     * shared frame-boundary state to corrupt by doing so). This same engine
     * may be installed on multiple Scenes at once, each processed
     * concurrently as its own job.
     */
    coopa::job::JobEngine* jobs = nullptr;

    /**
     * @brief This call's deferred command buffer -- see coopa/scene/scene_commands.h.
     *
     * A system executing inline (worker_index == k_main_thread_index, the
     * top-level case Scene::update()/late_update() always pass) may write
     * through `commands` OR call a mutating Scene/SceneObject method
     * directly -- both are safe on the owner thread. A system that fans work
     * out across jobs (e.g. via
     * JobEngine::parallel_for()) must have each chunk record into
     * `scene.commands_for(chunk_ctx.worker_index)` (using ITS OWN
     * per-chunk JobContext::worker_index, not this outer FrameContext's)
     * instead of ever calling a mutating Scene/SceneObject method from
     * inside that chunk.
     */
    SceneCommandBuffer* commands = nullptr;

    /// @brief k_main_thread_index (from coopa/job/context.h) for the top-level
    ///        call; a system fanning out across jobs assigns its own per-chunk value.
    uint32_t worker_index = 0;
};

/**
 * @struct WarmupState
 * @brief What a system still has to build before its scene is worth showing.
 *
 * A streamed world is not finished when its scene activates: terrain is still meshing, water is
 * still baking, the navmesh has not been built. A host that wants to hold a loading screen up
 * until the world is actually there asks every installed system for this and waits for all of
 * them (see toy::core::SceneLoadStage::Warming).
 *
 * `participates` is what keeps the default harmless: a system that reports false is ignored
 * entirely rather than counted as instantly warm, so a scene warms on exactly the systems that
 * have something to warm.
 */
struct WarmupState {
    bool        participates = false;  ///< False: this system has nothing to warm; ignore it.
    bool        warm = true;           ///< True once its outstanding work is done.
    float       progress = 1.0f;       ///< 0..1, for a loading bar.
    int         remaining = 0;         ///< Items still outstanding, for a status line.
    const char* what = "";             ///< What it is building: "terrain", "water", "navigation".
    /// ISceneSystem::system_name(), filled in by Scene::warm_up_states() -- a system reporting
    /// its own state leaves this alone. A stable key for a host tracking one system across
    /// frames, where `what` is a label meant for a status line.
    const char* name = "";
};

/**
 * @class ISceneSystem
 * @brief A unit of per-frame scene work, run at a fixed point in the phase order.
 *
 * Register an instance with Scene::add_system(). Systems are not copyable or
 * default-constructed by the Scene — it only ever holds the unique_ptr it was
 * handed.
 */
class ISceneSystem {
public:
    virtual ~ISceneSystem() = default;

    /** @brief Called once when the system is registered via Scene::add_system(). */
    virtual void on_attach(Scene& scene) { (void)scene; }

    /** @brief Called once when the system is removed via Scene::remove_system(). */
    virtual void on_detach(Scene& scene) { (void)scene; }

    /**
     * @brief Does this system's per-frame work.
     *
     * Called with a live Scene& every time — a system must not cache the
     * reference it receives in on_attach() and use it later instead, since
     * nothing guarantees the same Scene address across a move.
     */
    virtual void execute(Scene& scene, const FrameContext& ctx) = 0;

    /** @brief Stable name used by Scene::find_system()/remove_system() and logging. */
    virtual const char* system_name() const = 0;

    /**
     * @brief Whether this system still runs while its Scene is not simulating.
     *
     * An editor holds its scene in a non-simulating state (Scene::set_simulating(false)):
     * gameplay, physics and behaviour walks stop, while systems that only keep what is
     * drawn consistent with the authored data -- transform resolve, terrain meshing --
     * keep running. Default false: a system is gameplay unless it says otherwise.
     */
    virtual bool runs_in_edit_mode() const { return false; }

    /**
     * @brief What this system still has to build before the scene is worth showing.
     *
     * Default: participates in nothing, so a system that streams nothing needs no override and
     * never holds a loading screen up. Called once per frame while a scene is warming, so it
     * must be a cheap read of state the system already keeps -- never work of its own.
     */
    virtual WarmupState warm_up_state() const { return {}; }
};

/**
 * @class BehaviourSystem
 * @brief Built-in UpdatePhase::Behaviour step: the recursive Component::update() walk.
 *
 * Auto-installed by every Scene at construction. Remove it via
 * Scene::remove_system("Behaviour") if an application wants to replace the
 * built-in walk with something else.
 */
class BehaviourSystem : public ISceneSystem {
public:
    void execute(Scene& scene, const FrameContext& ctx) override;
    const char* system_name() const override { return "Behaviour"; }
};

/**
 * @class LateBehaviourSystem
 * @brief Built-in UpdatePhase::LateBehaviour step: the recursive Component::late_update() walk.
 *
 * Auto-installed by every Scene at construction.
 */
class LateBehaviourSystem : public ISceneSystem {
public:
    void execute(Scene& scene, const FrameContext& ctx) override;
    const char* system_name() const override { return "LateBehaviour"; }
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_SYSTEM_H
