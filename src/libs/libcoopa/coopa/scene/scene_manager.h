/**
 * @file scene_manager.h
 * @brief Owns and manages zero or more concurrently-processable Scenes.
 *
 * Provides scene loading via SceneLoader and delegates per-frame updates to
 * every active scene -- as one job per scene, run concurrently, when a
 * JobEngine is installed and there is more than one active scene; serially,
 * in registration order, otherwise (identical observable behavior either
 * way, and no threads are spawned unless a JobEngine was installed). Each
 * Scene is single-owner (see Scene's class doc), so distinct Scenes may
 * safely run on different worker threads at the same instant.
 */

#ifndef COOPA_SCENE_SCENE_MANAGER_H
#define COOPA_SCENE_SCENE_MANAGER_H

#include <coopa/job/parallel_for.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace coopa {
namespace scene {

/**
 * @class SceneManager
 * @brief Manages the lifecycle of zero or more Scenes, one of which is "active"
 *        (the primary scene get_active_scene()/add_system() etc. address).
 *
 * Single-scene usage:
 * @code
 * SceneManager mgr;
 * mgr.load_scene("assets/scenes/cube/scene.yaml");
 * // Per frame:
 * mgr.update(delta_time);
 * auto& scene = mgr.get_active_scene();
 * @endcode
 *
 * Multi-scene usage:
 * @code
 * SceneManager mgr;
 * mgr.set_job_engine(&engine);
 * Scene* menu  = mgr.load_scene("assets/scenes/menu.yaml");       // replaces all, becomes active
 * Scene* world = mgr.load_scene_additive("assets/scenes/world.yaml"); // added alongside
 * // Per frame: menu and world each run as their own job, concurrently.
 * mgr.begin_frame();
 * mgr.update(delta_time);
 * mgr.late_update(delta_time);
 * mgr.end_frame();
 * @endcode
 */
class SceneManager {
public:
    SceneManager() = default;

    /**
     * @brief Loads a scene from the given path, replacing every previously
     *        managed scene (the single-scene-app entry point).
     *
     * Registered systems are NOT carried across from a replaced scene —
     * re-register systems after each load_scene() call. Use
     * load_scene_additive() to add a scene without discarding the others.
     *
     * @param path Path to the scene file.
     * @return Non-owning pointer to the newly loaded (and now active) scene.
     * @throws std::runtime_error on load failure.
     */
    Scene* load_scene(const std::string& path) {
        scenes_.clear();
        active_index_ = kNoActive;
        return load_scene_additive(path);
    }

    /**
     * @brief Loads a scene from the given path and adds it alongside any
     *        already-managed scenes, active by default.
     *
     * @return Non-owning pointer to the newly loaded scene.
     * @throws std::runtime_error on load failure.
     */
    Scene* load_scene_additive(const std::string& path) {
        return add_scene(std::make_unique<Scene>(SceneLoader::load(path)));
    }

    /**
     * @brief Adds an already-constructed scene (e.g. built up by hand rather
     *        than via SceneLoader), active by default.
     *
     * Applies whatever JobEngine was installed via set_job_engine(). If this
     * is the first scene added (or the only one left after every other was
     * removed), it also becomes the active scene.
     *
     * @return Non-owning pointer to the added scene.
     */
    Scene* add_scene(std::unique_ptr<Scene> scene) {
        scene->set_job_engine(jobs_);
        Scene* raw = scene.get();
        scenes_.push_back(Entry{std::move(scene), true});
        if (active_index_ == kNoActive) active_index_ = scenes_.size() - 1;
        return raw;
    }

    /**
     * @brief Removes and destroys a managed scene by pointer.
     * @return True if `scene` was found and removed.
     */
    bool remove_scene(Scene* scene) {
        auto it = std::find_if(scenes_.begin(), scenes_.end(),
            [scene](const Entry& e) { return e.scene.get() == scene; });
        if (it == scenes_.end()) return false;

        size_t idx = static_cast<size_t>(it - scenes_.begin());
        scenes_.erase(it);

        if (active_index_ == idx) {
            active_index_ = scenes_.empty() ? kNoActive : 0;
        } else if (active_index_ != kNoActive && active_index_ > idx) {
            --active_index_;
        }
        return true;
    }

    /** @brief Enables/disables a managed scene's participation in update()/late_update(). */
    void set_scene_active(Scene* scene, bool active) {
        for (auto& e : scenes_) {
            if (e.scene.get() == scene) { e.active = active; return; }
        }
    }

    /** @brief Every managed scene, in registration order (regardless of active flag). */
    std::vector<Scene*> scenes() const {
        std::vector<Scene*> result;
        result.reserve(scenes_.size());
        for (const auto& e : scenes_) result.push_back(e.scene.get());
        return result;
    }

    /**
     * @brief Installs the JobEngine to hand to every managed scene (now and
     *        future). See Scene::set_job_engine() -- the same engine may be
     *        shared across every scene this manager holds; each still
     *        processes as an independent job.
     */
    void set_job_engine(coopa::job::JobEngine* engine) {
        jobs_ = engine;
        for (auto& e : scenes_) e.scene->set_job_engine(jobs_);
    }

    /** @brief Forwards to the active scene's add_system(). @throws std::runtime_error if no scene is loaded. */
    ISceneSystem* add_system(std::unique_ptr<ISceneSystem> system, UpdatePhase phase) {
        return get_active_scene().add_system(std::move(system), phase);
    }

    /** @brief Forwards to the active scene's remove_system(). @throws std::runtime_error if no scene is loaded. */
    bool remove_system(const std::string& system_name) {
        return get_active_scene().remove_system(system_name);
    }

    /** @brief Forwards to the active scene's find_system(). @throws std::runtime_error if no scene is loaded. */
    ISceneSystem* find_system(const std::string& system_name) const {
        return get_active_scene().find_system(system_name);
    }

    /** @brief Returns true if a scene is currently loaded/active. */
    bool has_scene() const { return active_index_ != kNoActive; }

    /**
     * @brief Makes an already-managed scene the primary one get_active_scene() returns.
     * @return False if `scene` is not managed by this SceneManager.
     */
    bool set_active_scene(Scene* scene) {
        for (size_t i = 0; i < scenes_.size(); ++i) {
            if (scenes_[i].scene.get() == scene) {
                active_index_ = i;
                return true;
            }
        }
        return false;
    }

    /** @brief True if `scene` participates in update()/late_update() (see set_scene_active()). */
    bool is_scene_active(const Scene* scene) const {
        for (const auto& e : scenes_) {
            if (e.scene.get() == scene) return e.active;
        }
        return false;
    }

    /** @brief Returns a reference to the active scene. @throws std::runtime_error if no scene is loaded. */
    Scene& get_active_scene() {
        if (active_index_ == kNoActive) throw std::runtime_error("[SceneManager] No active scene loaded.");
        return *scenes_[active_index_].scene;
    }

    /** @brief Returns a const reference to the active scene. */
    const Scene& get_active_scene() const {
        if (active_index_ == kNoActive) throw std::runtime_error("[SceneManager] No active scene loaded.");
        return *scenes_[active_index_].scene;
    }

    /// @brief Resets per-frame diagnostic counters on the installed JobEngine, if any.
    void begin_frame() {
        if (jobs_) jobs_->begin_frame();
    }

    /// @brief Reserved for future per-frame finalization on the installed JobEngine, if any.
    void end_frame() {
        if (jobs_) jobs_->end_frame();
    }

    /**
     * @brief Calls update() on every active scene.
     *
     * If a JobEngine is installed and more than one scene is active, each
     * active scene's update() runs as its own job, all concurrently, and
     * this call blocks until every one has finished. Otherwise every active
     * scene's update() runs inline, serially, in registration order --
     * identical observable behavior to the concurrent path, just without
     * the parallelism (and, with no engine installed, without spawning any
     * threads at all).
     *
     * @param delta_time Frame delta time in seconds.
     */
    void update(float delta_time) {
        run_phase_([delta_time](Scene& s) { s.update(delta_time); });
    }

    /**
     * @brief Calls late_update() on every active scene (each scene's own
     *        late_update() also flushes that scene's deferred
     *        SceneCommandBuffers -- see Scene::flush_commands()).
     *
     * Same concurrency shape as update() -- call once per frame, after
     * update(delta_time).
     *
     * @param delta_time Frame delta time in seconds.
     */
    void late_update(float delta_time) {
        run_phase_([delta_time](Scene& s) { s.late_update(delta_time); });
    }

private:
    struct Entry {
        std::unique_ptr<Scene> scene;
        bool active = true;
    };

    static constexpr size_t kNoActive = static_cast<size_t>(-1);

    std::vector<Scene*> active_scenes_() const {
        std::vector<Scene*> result;
        result.reserve(scenes_.size());
        for (const auto& e : scenes_) {
            if (e.active) result.push_back(e.scene.get());
        }
        return result;
    }

    /// @brief Runs `phase` (Scene::update or Scene::late_update, already bound to delta_time)
    ///        against every active scene, concurrently via one job per scene when
    ///        possible, else serially in registration order.
    template<typename Phase>
    void run_phase_(Phase&& phase) {
        std::vector<Scene*> active = active_scenes_();
        if (active.empty()) return;

        if (jobs_ && active.size() > 1) {
            // Normal help floor: a scene phase is frame-critical, so a thread waiting here must
            // not run a background Low job (a terrain chunk mesh, an asset bake) mid-wait.
            jobs_->parallel_for_blocking(active.size(), 1,
                [&active, &phase](size_t start, size_t end) {
                    for (size_t i = start; i < end; ++i) phase(*active[i]);
                },
                0, coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        } else {
            for (Scene* s : active) phase(*s);
        }
    }

    std::vector<Entry> scenes_;
    size_t                 active_index_ = kNoActive;
    coopa::job::JobEngine*  jobs_ = nullptr; /**< Non-owning; re-applied to each newly added scene. */
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_MANAGER_H
