/**
 * @file scene.h
 * @brief Root of the scene hierarchy, owning root SceneObjects.
 *
 * A Scene contains the named root objects. It carries no knowledge of any
 * particular component type (camera, light, mesh renderer, ...) — those are
 * defined outside libcoopa (e.g. gfxcoopa's engine::components), and are
 * reached generically via get_components<T>() / find_first_component<T>().
 * update() processes all root objects (and their children recursively) and
 * can be dispatched as a job on a worker thread via libcoopa's JobEngine.
 */

#ifndef COOPA_SCENE_SCENE_H
#define COOPA_SCENE_SCENE_H

#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <coopa/scene/scene_commands.h>
#include <coopa/event/event_bus.h>
#include <coopa/job/engine.h>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <utility>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <typeindex>
#include <unordered_map>

#ifdef COOPA_SCENE_THREAD_CHECKS
/// @brief Declares a scoped ownership check at the top of a Scene method.
/// Expands to nothing (zero cost) when COOPA_SCENE_THREAD_CHECKS is off.
#define COOPA_SCENE_ACCESS_GUARD(scene_ref) \
    ::coopa::scene::SceneAccessGuard coopa_scene_access_guard_(scene_ref)
#else
#define COOPA_SCENE_ACCESS_GUARD(scene_ref) ((void)0)
#endif

namespace coopa {
namespace scene {

class Scene; // Forward declaration, needed by SceneAccessGuard's `Scene&` member below.

#ifdef COOPA_SCENE_THREAD_CHECKS
/**
 * @class SceneAccessGuard
 * @brief RAII single-owner check: asserts no two threads are ever inside the
 *        same Scene's methods concurrently. See COOPA_SCENE_ACCESS_GUARD and
 *        Scene's class doc. Reentrant for the same thread (a system calling
 *        back into another Scene method on the same call stack is fine).
 *
 * Declared here (a reference to Scene is enough for that -- Scene need only
 * be forward-declared); constructor/destructor are defined out-of-line,
 * after Scene is a complete type, alongside this file's other out-of-line
 * definitions (BehaviourSystem::execute() etc.).
 */
class SceneAccessGuard {
public:
    explicit SceneAccessGuard(Scene& scene);
    ~SceneAccessGuard();
    SceneAccessGuard(const SceneAccessGuard&) = delete;
    SceneAccessGuard& operator=(const SceneAccessGuard&) = delete;
private:
    Scene& scene_;
};
#endif

/**
 * @class Scene
 * @brief Owns the root SceneObjects and provides generic hierarchy-wide queries.
 *
 * **Single-owner + deferred commands.** At any instant, at most one thread
 * may call a non-const method on a given Scene -- this is what lets an
 * engine process several independent Scenes concurrently (one job per
 * Scene) without any locking inside Scene itself: distinct Scene instances
 * share no state. Build with -DCOOPA_SCENE_THREAD_CHECKS=ON (see
 * CMakeLists.txt) to get a runtime assertion if two threads ever call into
 * the same Scene concurrently.
 *
 * Work fanned out *within* one Scene's update (e.g. a system's
 * parallel_for() over its components) must only READ scene state from a
 * worker job -- every write goes through that worker's SceneCommandBuffer
 * (see FrameContext::commands / Scene::commands_for(), and scene_commands.h)
 * instead of calling a mutating Scene/SceneObject method directly. The owner
 * thread applies every buffer's recorded commands, in worker-index order,
 * via flush_commands() -- call it once per frame, after every system has
 * run (late_update() already does this for you).
 *
 * Constructed by SceneLoader (or built up manually via add_root_object()).
 * update()/late_update() run an ordered pipeline of ISceneSystem instances
 * (see scene_system.h) — the recursive Component::update()/late_update()
 * walks are just the two built-in systems (BehaviourSystem,
 * LateBehaviourSystem), auto-installed by every Scene at construction.
 * get_components<T>() / find_objects_with<T>() / find_first_component<T>()
 * give the render pipeline (or any other system) a flat view over whichever
 * component types it cares about, without Scene needing to know those types
 * itself.
 */
class Scene {
public:
    /**
     * @brief Constructs an empty scene with the given name.
     *
     * Installs the two built-in systems (BehaviourSystem at
     * UpdatePhase::Behaviour, LateBehaviourSystem at UpdatePhase::LateBehaviour)
     * so update()/late_update() behave exactly as they did before the system
     * pipeline existed, with nothing else registered.
     *
     * @param name Scene name (e.g. from the YAML scene_name field).
     */
    explicit Scene(std::string name = "Scene")
        : name_(std::move(name))
    {
        install_builtin_systems_();
    }

    // Non-copyable, movable. Move operations re-stamp every component's
    // Component::scene back-pointer to the NEW Scene address (see
    // restamp_scene_pointers_()) — without this, a Scene moved after start()
    // has run (e.g. SceneLoader::load()'s `return scene;`, then moved again
    // into SceneManager's unique_ptr<Scene>) would leave every component
    // pointing at a dangling, already-destroyed Scene. systems_/jobs_/
    // frame_index_/worker_commands_ move alongside root_objects_/events_ for
    // the same reason: this Scene may be mid-frame the moment it is
    // relocated. Per this class's single-owner contract, a move itself must
    // not race a concurrent call into either Scene -- same precondition as
    // any other move in this codebase.
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    Scene(Scene&& other) noexcept
        : name_(std::move(other.name_)),
          root_objects_(std::move(other.root_objects_)),
          events_(std::move(other.events_)),
          systems_(std::move(other.systems_)),
          jobs_(other.jobs_),
          simulating_(other.simulating_),
          frame_index_(other.frame_index_.load(std::memory_order_relaxed)),
          worker_commands_(std::move(other.worker_commands_))
    {
        other.jobs_ = nullptr;
        // The component cache is NOT moved: every SceneObject's structure hook must point at
        // THIS Scene's counter (restamp below), and the fresh cache simply rebuilds on first
        // query. structure_revision_ starts ahead of every (empty) cache entry.
        restamp_scene_pointers_();
    }

    Scene& operator=(Scene&& other) noexcept {
        if (this == &other) return *this;
        name_             = std::move(other.name_);
        root_objects_     = std::move(other.root_objects_);
        events_           = std::move(other.events_);
        systems_          = std::move(other.systems_);
        jobs_             = other.jobs_;
        simulating_       = other.simulating_;
        frame_index_.store(other.frame_index_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        worker_commands_  = std::move(other.worker_commands_);
        other.jobs_ = nullptr;
        {
            // Entries cached for the hierarchy this Scene owned before the assignment.
            std::lock_guard<std::mutex> lock(component_cache_mutex_);
            component_cache_.clear();
        }
        structure_revision_.fetch_add(1, std::memory_order_relaxed);
        restamp_scene_pointers_();
        return *this;
    }

    // --- Accessors ---

    /** @brief Returns the scene name. */
    const std::string& name() const { return name_; }

    /**
     * @brief Adds a root SceneObject (takes ownership).
     * @param obj The root object to add.
     * @return Raw pointer to the added object.
     */
    SceneObject* add_root_object(std::unique_ptr<SceneObject> obj) {
        COOPA_SCENE_ACCESS_GUARD(*this);
        SceneObject* raw = obj.get();
        root_objects_.push_back(std::move(obj));
        raw->set_structure_hook(&structure_revision_);
        bump_structure_revision();
        return raw;
    }

    /**
     * @brief Detaches and destroys a root object by pointer.
     * @return True if `obj` was found among root_objects() and removed.
     */
    bool remove_root_object(SceneObject* obj) {
        COOPA_SCENE_ACCESS_GUARD(*this);
        auto it = std::find_if(root_objects_.begin(), root_objects_.end(),
            [obj](const std::unique_ptr<SceneObject>& o) { return o.get() == obj; });
        if (it == root_objects_.end()) return false;
        root_objects_.erase(it);
        bump_structure_revision();
        return true;
    }

    /** @brief Returns the list of root SceneObjects (read-only). */
    const std::vector<std::unique_ptr<SceneObject>>& root_objects() const {
        return root_objects_;
    }

    /**
     * @brief Returns the list of root SceneObjects (mutable).
     *
     * Adding or removing entries directly through this reference bypasses the component query
     * cache -- call bump_structure_revision() afterwards (add_root_object() /
     * remove_root_object() already do).
     */
    std::vector<std::unique_ptr<SceneObject>>& root_objects() {
        return root_objects_;
    }

    // --- Lifecycle ---

    /**
     * @brief Calls start() on all root objects (recursively).
     *
     * Should be called once after the scene is fully loaded. Also stamps
     * every component's Component::scene back-pointer to this Scene (just
     * before that component's own start() runs), so start()/update()/
     * late_update() can all reach scene->events() from within a component.
     */
    void start() {
        COOPA_SCENE_ACCESS_GUARD(*this);
        restamp_scene_pointers_();
        for (auto& obj : root_objects_) obj->start();
    }

    /**
     * @brief Runs every registered system with order < UpdatePhase::LateBehaviour.
     *
     * With nothing else registered this is just the built-in BehaviourSystem
     * (the recursive Component::update() walk), making it equivalent to a
     * direct walk of the Component tree.
     *
     * A JobEngine installed via set_job_engine() is handed to every system via
     * FrameContext::jobs. This Scene never touches that engine's frame
     * boundary, so one engine may be freely shared across many
     * concurrently-processing Scenes and other subsystems.
     *
     * Safe to call from any thread, but only ONE thread at a time for a
     * given Scene -- see this class's single-owner doc. Multiple distinct
     * Scenes may each have update() running concurrently on their own
     * thread/job.
     *
     * @param delta_time Frame delta time in seconds.
     */
    void update(float delta_time) {
        COOPA_SCENE_ACCESS_GUARD(*this);
        ensure_command_buffers_();
        uint32_t widx = coopa::job::k_main_thread_index;
        FrameContext ctx{delta_time, frame_index_.load(std::memory_order_relaxed), jobs_,
                         &commands_for(widx), widx};
        for (auto& entry : systems_) {
            if (entry.order < static_cast<int>(UpdatePhase::LateBehaviour) &&
                (simulating_ || entry.system->runs_in_edit_mode())) {
                execute_timed_(entry, ctx);
            }
        }
    }

    /**
     * @brief Runs every registered system with order >= UpdatePhase::LateBehaviour,
     *        then flushes every worker's deferred SceneCommandBuffer.
     *
     * With nothing else registered the system pipeline here is just the
     * built-in LateBehaviourSystem (the recursive Component::late_update()
     * walk) — call once per frame, after update(dt), exactly as before.
     * Still valid to call standalone with no preceding update() this frame
     * (several existing tests do).
     *
     * flush_commands() runs last, after every system (both phases) has had
     * its chance to record commands this frame — see flush_commands()'s doc.
     *
     * @param delta_time Frame delta time in seconds.
     */
    void late_update(float delta_time) {
        COOPA_SCENE_ACCESS_GUARD(*this);
        ensure_command_buffers_();
        uint32_t widx = coopa::job::k_main_thread_index;
        FrameContext ctx{delta_time, frame_index_.load(std::memory_order_relaxed), jobs_,
                         &commands_for(widx), widx};
        for (auto& entry : systems_) {
            if (entry.order >= static_cast<int>(UpdatePhase::LateBehaviour) &&
                (simulating_ || entry.system->runs_in_edit_mode())) {
                execute_timed_(entry, ctx);
            }
        }
        flush_commands();
        frame_index_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Applies every worker's recorded SceneCommandBuffer, in
     *        worker-index order (deterministic), then clears them.
     *
     * Called automatically at the end of late_update() — call it directly
     * only if commands were recorded after late_update() already ran this
     * frame (uncommon). Must run on the owning thread, after every system
     * that might still record a command has finished (a job must never
     * still be running when this executes -- the caller is responsible for
     * having already waited on any outstanding job handles).
     */
    void flush_commands() {
        COOPA_SCENE_ACCESS_GUARD(*this);
        for (auto& buffer : worker_commands_) {
            if (!buffer.empty()) buffer.flush_(*this);
        }
    }

    /**
     * @brief Returns the SceneCommandBuffer a system should record deferred
     *        edits into for the given worker index (see FrameContext::worker_index).
     *
     * Sized to jobs_->worker_count() + 1 slots: indices [0, worker_count())
     * are the JobEngine's own worker indices, and the last slot is reserved
     * for coopa::job::k_main_thread_index (the owner thread calling in
     * directly, not from inside a job) -- update()/late_update() always pass
     * k_main_thread_index for the outermost FrameContext they build; a
     * system that internally fans out via parallel_for() should instead look
     * up commands_for() using ITS OWN per-chunk JobContext::worker_index.
     */
    SceneCommandBuffer& commands_for(uint32_t worker_index) {
        ensure_command_buffers_();
        size_t last = worker_commands_.size() - 1;
        size_t idx = (worker_index == coopa::job::k_main_thread_index || worker_index > last)
                         ? last : static_cast<size_t>(worker_index);
        return worker_commands_[idx];
    }

    /**
     * @brief Stamps Component::scene on every component in obj's subtree,
     *        without calling start() on any of them.
     *
     * For attaching a subtree to this Scene after start() has already run
     * once (e.g. dynamically spawning a new SceneObject mid-game) — the
     * ordinary path (a subtree present at load time) is already covered by
     * start(). Idempotent; safe to call on a subtree that already belongs to
     * this Scene.
     *
     * @param obj Root of the subtree to adopt. Must already be reachable
     *            from this Scene's root_objects() (e.g. via add_root_object()
     *            or SceneObject::add_child()) — this only stamps the
     *            back-pointer, it does not change ownership.
     */
    void adopt(SceneObject& obj) {
        COOPA_SCENE_ACCESS_GUARD(*this);
        obj.for_each_recursive([this](SceneObject& o) {
            for (auto& comp : o.components()) comp->scene = this;
        });
        obj.set_structure_hook(&structure_revision_);
        bump_structure_revision();
    }

    /** @brief The scene-wide named EventBus — see coopa/event/event_bus.h. */
    coopa::event::EventBus& events() { return events_; }

    // --- System pipeline ---

    /**
     * @brief Installs the JobEngine every system sees via FrameContext::jobs
     *        (non-owning; the caller keeps ownership).
     *
     * This Scene needs no exclusive ownership of the engine's frame boundary:
     * JobHandle slots are reclaimed individually (see coopa/job/handle.h), so
     * the same engine may be shared across this Scene, other concurrently-
     * processing Scenes, coopa::asset::AssetManager, and anything else, with
     * no coordination required between them.
     *
     * Passing nullptr (the default state) makes every system run inline on
     * the calling thread — no threads are spawned unless this is called.
     *
     * @param engine Non-owning pointer to the JobEngine to drive, or nullptr.
     */
    void set_job_engine(coopa::job::JobEngine* engine) {
        jobs_ = engine;
        // Sized here (not just lazily in update()) so worker_commands_ never
        // reallocates mid-frame -- a system that calls commands_for() while
        // another system's FrameContext::commands (a raw pointer into this
        // same vector) is still in scope must never see that pointer
        // invalidated by a resize.
        ensure_command_buffers_();
    }

    /** @brief Returns the installed frame JobEngine, or nullptr if none. */
    coopa::job::JobEngine* job_engine() const { return jobs_; }

    /**
     * @brief Registers a system at the given phase slot.
     *
     * Systems at equal order run in registration order. Ownership transfers
     * to the Scene; on_attach() is called immediately.
     *
     * @return Non-owning pointer to the registered system.
     */
    ISceneSystem* add_system(std::unique_ptr<ISceneSystem> system, UpdatePhase phase) {
        return add_system(std::move(system), static_cast<int>(phase));
    }

    /**
     * @brief Registers a system at an arbitrary numeric order.
     *
     * Lets a consumer insert between the built-in phases (e.g. 375 for IK,
     * which would run after coopa::scene::TransformSystem(350) but still
     * inside update() since it is below LateBehaviour(400)).
     *
     * @return Non-owning pointer to the registered system.
     */
    ISceneSystem* add_system(std::unique_ptr<ISceneSystem> system, int order) {
        COOPA_SCENE_ACCESS_GUARD(*this);
        ISceneSystem* raw = system.get();
        systems_.push_back(SystemEntry{order, std::move(system), 0.0});
        std::stable_sort(systems_.begin(), systems_.end(),
            [](const auto& a, const auto& b) { return a.order < b.order; });
        raw->on_attach(*this);
        return raw;
    }

    /**
     * @brief Removes the system with the given system_name(), calling on_detach().
     * @return True if a matching system was found and removed.
     */
    bool remove_system(const std::string& system_name) {
        auto it = std::find_if(systems_.begin(), systems_.end(),
            [&](const auto& entry) { return system_name == entry.system->system_name(); });
        if (it == systems_.end()) return false;
        it->system->on_detach(*this);
        systems_.erase(it);
        return true;
    }

    /** @brief Finds a registered system by system_name(), or nullptr. */
    ISceneSystem* find_system(const std::string& system_name) const {
        for (const auto& entry : systems_) {
            if (system_name == entry.system->system_name()) return entry.system.get();
        }
        return nullptr;
    }

    /** @brief One registered system's execute() time from the current frame, milliseconds. */
    struct SystemTiming {
        const char* name;  /**< ISceneSystem::system_name(). */
        double      ms;    /**< Owner-thread milliseconds of the most recent execute(). */
    };

    /**
     * @brief Owner-thread milliseconds each registered system's most recent execute() took, in
     *        pipeline order -- both update() and late_update() phases. A system that did not run
     *        this frame (edit mode, wrong phase) reports its last value; 0 before its first run.
     */
    std::vector<SystemTiming> system_timings() const {
        std::vector<SystemTiming> out;
        out.reserve(systems_.size());
        for (const auto& entry : systems_) {
            out.push_back(SystemTiming{entry.system->system_name(), entry.last_execute_ms});
        }
        return out;
    }

    /**
     * @brief Every registered system's ISceneSystem::warm_up_state(), in pipeline order.
     *
     * For a host holding a loading screen up until a streamed world is actually built. Systems
     * that stream nothing report `participates == false` and can simply be skipped. Each entry's
     * WarmupState::name is filled in here, so a host can follow one system across frames.
     */
    std::vector<WarmupState> warm_up_states() const {
        std::vector<WarmupState> out;
        out.reserve(systems_.size());
        for (const auto& entry : systems_) {
            out.push_back(entry.system->warm_up_state());
            out.back().name = entry.system->system_name();
        }
        return out;
    }

    /** @brief Monotonically increasing frame counter, incremented once per late_update(). */
    uint64_t frame_index() const { return frame_index_.load(std::memory_order_relaxed); }

    /**
     * @brief Turns simulation on (the default) or off.
     *
     * Off is an editor's "edit mode": update()/late_update() run only the systems whose
     * ISceneSystem::runs_in_edit_mode() is true (transform resolve, terrain meshing), so
     * physics, behaviour walks and animation stand still while the scene is still drawn
     * and its deferred commands still flush.
     */
    void set_simulating(bool simulating) { simulating_ = simulating; }

    /** @brief See set_simulating(). */
    bool is_simulating() const { return simulating_; }

    // --- Generic queries ---
    //
    // All three queries are served from a per-type cache keyed to structure_revision_: the full
    // hierarchy walk (and its dynamic_cast per component) runs only on the first query of a type
    // after a structural change. SceneObject bumps the revision through its structure hook on
    // every component add/remove, child attach/detach and active toggle, and Scene bumps it in
    // add_root_object()/remove_root_object()/adopt() -- so with a static hierarchy the walks
    // disappear from the frame entirely. The cache mutex keeps a read-only worker job's query
    // safe against the owner thread querying concurrently (both are reads of scene state, but a
    // miss writes the cache).

    /**
     * @brief Returns a flat list of every active component of type T in the scene hierarchy.
     * @tparam T Component type to search for.
     * @return Vector of non-owning T* pointers, in pre-order.
     */
    template<typename T>
    std::vector<T*> get_components() const {
        std::lock_guard<std::mutex> lock(component_cache_mutex_);
        const ComponentCacheEntry& entry = cached_entry_<T>();
        std::vector<T*> result;
        result.reserve(entry.components.size());
        // static_cast, not another dynamic_cast: each pointer was produced by
        // get_component<T>() during the cached walk, so its dynamic type is known.
        for (Component* c : entry.components) result.push_back(static_cast<T*>(c));
        return result;
    }

    /**
     * @brief Returns every active SceneObject that carries a component of type T.
     * @tparam T Component type to search for.
     * @return Vector of non-owning SceneObject pointers, in pre-order.
     */
    template<typename T>
    std::vector<SceneObject*> find_objects_with() const {
        std::lock_guard<std::mutex> lock(component_cache_mutex_);
        return cached_entry_<T>().objects;
    }

    /**
     * @brief Returns the first active component of type T found in the hierarchy (pre-order).
     * @tparam T Component type to search for.
     * @return Non-owning pointer to the first match, or nullptr.
     */
    template<typename T>
    T* find_first_component() const {
        std::lock_guard<std::mutex> lock(component_cache_mutex_);
        const ComponentCacheEntry& entry = cached_entry_<T>();
        return entry.components.empty() ? nullptr : static_cast<T*>(entry.components.front());
    }

    /**
     * @brief Invalidates the cached component queries after a structural change the Scene could
     *        not observe (e.g. entries added to or removed from the mutable root_objects()
     *        reference directly). The ordinary mutation paths -- SceneObject's component and
     *        child methods on an adopted object, add_root_object(), adopt(), set_active() --
     *        already do this.
     */
    void bump_structure_revision() {
        structure_revision_.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Finds the first SceneObject with the given name (depth-first).
     * @param name Name to search for.
     * @return Non-owning pointer to the found object, or nullptr.
     */
    SceneObject* find_object(const std::string& name) const {
        for (const auto& root : root_objects_) {
            if (root->name() == name) return root.get();
            if (SceneObject* found = root->find_descendant(name)) return found;
        }
        return nullptr;
    }

    /**
     * @brief Finds a SceneObject by a ':'-separated hierarchy path, e.g. "sdf_blob:sdf_blob_sphere".
     *
     * The FIRST segment is resolved with find_object() (name-anywhere, depth-first); each
     * subsequent segment must name a DIRECT child of the previous match (SceneObject::find_child()).
     * A single-segment path is therefore exactly equivalent to find_object(), which makes this a
     * strict superset -- a path is accepted anywhere a bare name is.
     * Resolving the head loosely is what lets "sdf_blob:sdf_blob_sphere" work whether or not
     * sdf_blob happens to be a root object.
     *
     * @param path ':'-separated object names. Empty segments (including a leading/trailing/doubled
     *             ':') and an empty path yield nullptr rather than skipping the segment.
     * @return Non-owning pointer to the object, or nullptr if any segment fails to resolve.
     */
    SceneObject* find_object_by_path(std::string_view path) const {
        if (path.empty()) return nullptr;

        // A trailing ':' (e.g. "a:") leaves an empty final segment once split below, which the
        // loop must reject rather than silently treat as "no more segments" -- check up front
        // rather than relying on an empty `rest` to mean two different things.
        if (path.back() == ':') return nullptr;

        size_t head_end = path.find(':');
        std::string_view head = (head_end == std::string_view::npos) ? path : path.substr(0, head_end);
        if (head.empty()) return nullptr;

        SceneObject* current = find_object(std::string(head));
        if (!current || head_end == std::string_view::npos) return current;

        std::string_view rest = path.substr(head_end + 1);
        while (!rest.empty()) {
            size_t seg_end = rest.find(':');
            std::string_view segment = (seg_end == std::string_view::npos) ? rest : rest.substr(0, seg_end);
            if (segment.empty()) return nullptr;

            current = current->find_child(segment);
            if (!current) return nullptr;

            if (seg_end == std::string_view::npos) break;
            rest = rest.substr(seg_end + 1);
        }
        return current;
    }

private:
    /** @brief One component type's cached query results -- see the queries' section doc. */
    struct ComponentCacheEntry {
        uint64_t                  revision = 0; /**< structure_revision_ the walk ran at. */
        std::vector<Component*>   components;   /**< First T on each matching object, pre-order. */
        std::vector<SceneObject*> objects;      /**< The objects those components sit on. */
    };

    /**
     * @brief The cached (and, when stale, rebuilt) query results for T. Caller holds
     *        component_cache_mutex_. The walk and filter are exactly the queries' documented
     *        semantics: pre-order over every root, skipping each inactive object itself (an
     *        inactive parent's active descendants are still visited), first component of type T
     *        per object.
     */
    template<typename T>
    const ComponentCacheEntry& cached_entry_() const {
        ComponentCacheEntry& entry = component_cache_[std::type_index(typeid(T))];
        const uint64_t revision = structure_revision_.load(std::memory_order_relaxed);
        // COOPA_SCENE_NOCACHE=1 re-walks on every query -- the diagnostic for a suspected
        // missing invalidation (a mutation path without a revision bump): behaviour that
        // changes with it points there.
        static const bool force_walk = std::getenv("COOPA_SCENE_NOCACHE") != nullptr;
        if (!force_walk && entry.revision == revision) return entry;
        entry.components.clear();
        entry.objects.clear();
        for (const auto& root : root_objects_) {
            root->for_each_recursive([&entry](const SceneObject& obj) {
                if (!obj.active()) return;
                if (auto* c = const_cast<SceneObject&>(obj).template get_component<T>()) {
                    entry.components.push_back(c);
                    entry.objects.push_back(const_cast<SceneObject*>(&obj));
                }
            });
        }
        entry.revision = revision;
        return entry;
    }

    /** @brief One registered system: its order slot, the system, and its last timing. */
    struct SystemEntry {
        int                           order;
        std::unique_ptr<ISceneSystem> system;
        double                        last_execute_ms = 0.0; /**< See system_timings(). */
    };

    /** @brief Runs one system's execute() and records its owner-thread milliseconds. */
    void execute_timed_(SystemEntry& entry, const FrameContext& ctx) {
        const auto start = std::chrono::steady_clock::now();
        entry.system->execute(*this, ctx);
        entry.last_execute_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    /**
     * @brief Stamps Component::scene = this on every component in the hierarchy.
     *
     * Regardless of active() — an initially-inactive object's components
     * should still be able to reach scene->events() once reactivated later
     * via set_active(true). Shared by start() and the move operations (a
     * moved-into Scene has a different address than the one that may have
     * already run start() before the move — see the move ctor/assignment doc).
     * Also points every object's structure hook at this Scene's revision
     * counter, for the component query cache.
     */
    void restamp_scene_pointers_() {
        for (auto& root : root_objects_) {
            root->for_each_recursive([this](SceneObject& obj) {
                for (auto& comp : obj.components()) comp->scene = this;
            });
            root->set_structure_hook(&structure_revision_);
        }
    }

    /** @brief Installs BehaviourSystem/LateBehaviourSystem. Called once, from every constructor path. */
    void install_builtin_systems_() {
        add_system(std::make_unique<BehaviourSystem>(), UpdatePhase::Behaviour);
        add_system(std::make_unique<LateBehaviourSystem>(), UpdatePhase::LateBehaviour);
    }

    /**
     * @brief Grows worker_commands_ to cover every JobEngine worker plus the
     *        reserved main-thread slot. Cheap to call every update()/
     *        late_update() -- a no-op once already sized correctly.
     */
    void ensure_command_buffers_() {
        size_t needed = static_cast<size_t>(jobs_ ? jobs_->worker_count() : 0) + 1;
        if (worker_commands_.size() < needed) worker_commands_.resize(needed);
    }

    std::string                               name_;         /**< Scene name. */
    std::vector<std::unique_ptr<SceneObject>> root_objects_; /**< Owned root objects. */
    coopa::event::EventBus                    events_;       /**< Scene-wide named signal bus. */

    /** @brief Registered systems, kept sorted ascending by order; equal orders keep insertion order. */
    std::vector<SystemEntry> systems_;
    coopa::job::JobEngine* jobs_ = nullptr; /**< Non-owning. See set_job_engine(). */
    bool                   simulating_ = true; /**< See set_simulating(). */
    std::atomic<uint64_t>  frame_index_{0}; /**< Bumped once per late_update(); atomic so a worker job may read it. */

    /// @brief Per-worker deferred command buffers -- see commands_for()/flush_commands().
    std::vector<SceneCommandBuffer> worker_commands_;

    /// @brief Bumped on every structural change (see bump_structure_revision()); every adopted
    /// SceneObject's structure hook points here. Atomic: workers read it through the hook, and
    /// starts at 1 so a default ComponentCacheEntry (revision 0) is always stale.
    mutable std::atomic<uint64_t> structure_revision_{1};
    /// @brief Cached query results per component type -- see cached_entry_(). Mutable: queries
    /// are const. Guarded by component_cache_mutex_ (a worker job may query concurrently with
    /// the owner thread; neither mutex member takes part in a Scene move).
    mutable std::unordered_map<std::type_index, ComponentCacheEntry> component_cache_;
    mutable std::mutex component_cache_mutex_;

#ifdef COOPA_SCENE_THREAD_CHECKS
    friend class SceneAccessGuard;
    std::atomic<std::thread::id> owner_thread_{};
    int reentrancy_depth_ = 0; /**< Only ever touched by whichever thread currently owns owner_thread_. */
#endif
};

#ifdef COOPA_SCENE_THREAD_CHECKS
// SceneAccessGuard's constructor/destructor, defined out-of-line now that
// Scene is a complete type (its class shell is declared earlier in this file).
inline SceneAccessGuard::SceneAccessGuard(Scene& scene) : scene_(scene) {
    std::thread::id this_id = std::this_thread::get_id();
    std::thread::id expected{};
    if (scene_.owner_thread_.compare_exchange_strong(expected, this_id, std::memory_order_acq_rel)) {
        scene_.reentrancy_depth_ = 1;
    } else {
        assert(expected == this_id &&
               "Scene accessed concurrently from more than one thread -- "
               "see Scene's single-owner + deferred-commands contract");
        ++scene_.reentrancy_depth_;
    }
}
inline SceneAccessGuard::~SceneAccessGuard() {
    if (--scene_.reentrancy_depth_ == 0) {
        scene_.owner_thread_.store(std::thread::id{}, std::memory_order_release);
    }
}
#endif // COOPA_SCENE_THREAD_CHECKS

// Defined out-of-line, after Scene is a complete type — these are the two
// built-in ISceneSystem bodies declared in scene_system.h.

inline void BehaviourSystem::execute(Scene& scene, const FrameContext& ctx) {
    for (auto& obj : scene.root_objects()) obj->update(ctx.delta_time);
}

inline void LateBehaviourSystem::execute(Scene& scene, const FrameContext& ctx) {
    for (auto& obj : scene.root_objects()) obj->late_update(ctx.delta_time);
}

// SceneCommandBuffer::flush_() is defined out-of-line here too, for the same
// reason -- it needs Scene as a complete type.
inline void SceneCommandBuffer::flush_(Scene& scene) {
    for (auto& op : ops_) {
        std::visit([&scene](auto& o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, detail::AddRootObjectOp>) {
                SceneObject* raw = scene.add_root_object(std::move(o.obj));
                scene.adopt(*raw);
            } else if constexpr (std::is_same_v<T, detail::AddChildOp>) {
                SceneObject* raw = o.parent->add_child(std::move(o.child));
                scene.adopt(*raw);
            } else if constexpr (std::is_same_v<T, detail::DestroyObjectOp>) {
                if (SceneObject* parent = o.obj->parent()) {
                    parent->detach_child(o.obj); // returned unique_ptr destroyed immediately
                } else {
                    scene.remove_root_object(o.obj);
                }
            } else if constexpr (std::is_same_v<T, detail::AttachComponentOp>) {
                o.obj->attach_component(std::move(o.comp));
            } else if constexpr (std::is_same_v<T, detail::RemoveComponentOp>) {
                if (o.comp->owner) o.comp->owner->remove_component(o.comp);
            } else if constexpr (std::is_same_v<T, detail::SetActiveOp>) {
                o.obj->set_active(o.active);
            } else if constexpr (std::is_same_v<T, detail::EmitOp>) {
                scene.events().emit(o.object, o.signal, o.args);
            } else if constexpr (std::is_same_v<T, detail::CustomOp>) {
                o.fn(scene);
            }
        }, op);
    }
    ops_.clear();
}

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_H
