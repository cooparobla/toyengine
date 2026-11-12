/**
 * @file runner.h
 * @brief RoutineRunner -- the per-tick pump that owns and resumes running
 *        Routines -- plus the RoutineHandle token and the RAII RoutineScope.
 *
 * A routine body always resumes on the thread that calls tick(). The runner
 * never fans routine resumption out across the JobEngine, so a routine body may
 * touch a Scene exactly as freely as `Component::update()` may. The engine is
 * used only to run work a routine *awaits* -- see `on_worker()` and
 * `wait_for()` in yield.h.
 *
 * Re-entrancy follows the same rules coopa::event::Signal already uses for
 * connect/disconnect during emit(): starting a routine from inside a routine
 * body is safe, and stopping one -- including itself -- is safe. A coroutine
 * frame is never destroyed while it is executing.
 */

#ifndef COOPA_ROUTINE_RUNNER_H
#define COOPA_ROUTINE_RUNNER_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/routine/routine.h>
#include <coopa/routine/yield.h>

// Forward declaration only -- the Component-owning overloads of
// RoutineScope::start() are defined in coopa/routine/routine_system.h, which is
// the header that may depend on coopa/scene. runner.h itself stays free of it.
namespace coopa {
namespace scene {
class Component;
} // namespace scene
} // namespace coopa

namespace coopa {
namespace routine {

/// @brief Per-runner routine identifier, handed out monotonically and never reused.
using RoutineId = uint64_t;

/// @brief Never returned by a successful start(); marks an empty RoutineHandle.
inline constexpr RoutineId k_invalid_routine = 0;

class RoutineRunner;

namespace detail {

/**
 * @struct RunnerCore
 * @brief Shared control block linking a RoutineRunner to every handle it handed out.
 *
 * The runner owns the only shared_ptr; every RoutineHandle holds a weak_ptr.
 * ~RoutineRunner() clears `runner`, so a handle that outlives its runner
 * degrades to "not running" rather than dangling -- exactly what
 * coopa::event::Connection does for Signal.
 */
struct RunnerCore {
    RoutineRunner* runner = nullptr; /**< Owning runner, or null once it has been destroyed. */
};

/**
 * @struct RoutineState
 * @brief One running routine: its nesting stack, what it is waiting for, and
 *        its bookkeeping flags.
 *
 * `stack.back()` is the coroutine that actually resumes. A `co_yield` of
 * another Routine pushes onto this stack and a completed child pops off it,
 * which is how Unity stacks nested enumerators.
 */
struct RoutineState {
    RoutineId id = k_invalid_routine;  /**< Identity this routine's handles refer to. */
    const void* owner = nullptr;       /**< Opaque owner token for stop_owner(), or null. */
    std::vector<Routine> stack;        /**< Nesting chain; back() is the active coroutine. */
    YieldInstruction wait;             /**< What the active coroutine is suspended on. */
    bool retired = false;              /**< Scheduled for removal by the next reap. */
    bool executing = false;            /**< True while inside resume(): the frame must not be destroyed. */
};

} // namespace detail

/**
 * @class RoutineHandle
 * @brief Copyable token identifying one running routine, safe to hold past its
 *        runner's lifetime.
 */
class RoutineHandle {
public:
    /// @brief Constructs an empty handle that refers to no routine.
    RoutineHandle() = default;

    /**
     * @brief The identity this handle refers to.
     * @return The routine id, or k_invalid_routine for an empty handle.
     */
    RoutineId id() const { return id_; }

    /**
     * @brief Checks whether the routine is still running.
     * @return True if the runner is alive and the routine has neither finished nor been stopped.
     */
    bool is_running() const;

    /**
     * @brief Stops the routine, destroying its remaining coroutine frames.
     *
     * A no-op if the routine already finished, was already stopped, or its
     * runner has been destroyed.
     *
     * @return True if a running routine was stopped by this call.
     */
    bool stop() const;

    /// @brief Same as a non-empty id().
    explicit operator bool() const { return id_ != k_invalid_routine; }

    /// @brief Two handles are equal when they name the same routine of the same runner.
    bool operator==(const RoutineHandle& other) const {
        return id_ == other.id_ && !core_.owner_before(other.core_) && !other.core_.owner_before(core_);
    }

    /// @brief Inequality.
    bool operator!=(const RoutineHandle& other) const { return !(*this == other); }

private:
    friend class RoutineRunner;

    RoutineHandle(std::weak_ptr<detail::RunnerCore> core, RoutineId id)
        : core_(std::move(core)), id_(id) {}

    std::weak_ptr<detail::RunnerCore> core_; /**< The runner's control block. */
    RoutineId id_ = k_invalid_routine;       /**< Identity within that runner. */
};

/**
 * @class RoutineRunner
 * @brief Owns a set of running Routines and resumes the eligible ones once per tick().
 *
 * Usage:
 * @code
 * coopa::routine::RoutineRunner runner(&engine);   // engine is optional
 * auto handle = runner.start(blink());
 * while (running) {
 *     runner.tick(delta_time);
 * }
 * @endcode
 *
 * Not thread-safe: start(), stop() and tick() on one runner must all be called
 * from the same thread. That is the whole point -- it is what lets a routine
 * body mutate a Scene directly.
 */
class RoutineRunner {
public:
    /**
     * @brief Constructs a runner.
     * @param jobs Non-owning JobEngine used by `on_worker()`, or nullptr to run
     *        every `on_worker()` body inline on the tick thread instead.
     */
    explicit RoutineRunner(coopa::job::JobEngine* jobs = nullptr)
        : core_(std::make_shared<detail::RunnerCore>()), jobs_(jobs), logger_("RoutineRunner") {
        core_->runner = this;
    }

    /// @brief Stops every routine still running, then severs every outstanding handle.
    ~RoutineRunner() {
        core_->runner = nullptr;
        stop_all();
    }

    /// @brief Non-copyable: outstanding handles and owner tokens refer to this address.
    RoutineRunner(const RoutineRunner&) = delete;
    /// @brief Non-copyable.
    RoutineRunner& operator=(const RoutineRunner&) = delete;
    /// @brief Non-movable, for the same reason.
    RoutineRunner(RoutineRunner&&) = delete;
    /// @brief Non-movable.
    RoutineRunner& operator=(RoutineRunner&&) = delete;

    /**
     * @brief Installs (or clears) the JobEngine that `on_worker()` bodies are submitted to.
     * @param jobs Non-owning engine pointer, or nullptr for inline execution.
     */
    void set_job_engine(coopa::job::JobEngine* jobs) { jobs_ = jobs; }

    /**
     * @brief The installed JobEngine, or nullptr.
     * @return Non-owning engine pointer.
     */
    coopa::job::JobEngine* job_engine() const { return jobs_; }

    /**
     * @brief Starts a routine, running its body immediately up to the first suspension.
     *
     * Matches Unity's StartCoroutine: everything before the first `co_yield`
     * has already run by the time this returns, so a routine whose body never
     * suspends is finished before the handle is handed back.
     *
     * Safe to call from inside a routine body: the new routine still starts
     * immediately, and joins the pump at the end of the current tick.
     *
     * @param r The routine to run. Ownership transfers to the runner.
     * @param owner Opaque token grouping this routine for stop_owner(), e.g. the
     *        address of the component that started it. May be nullptr.
     * @return A handle naming the routine, or an empty handle if `r` owned no frame.
     */
    RoutineHandle start(Routine&& r, const void* owner = nullptr) {
        if (!r.valid()) return RoutineHandle();

        auto state = std::make_unique<detail::RoutineState>();
        state->id = next_id_++;
        state->owner = owner;
        state->stack.push_back(std::move(r));

        detail::RoutineState* raw = state.get();
        index_[raw->id] = raw;
        (pumping_ ? pending_ : active_).push_back(std::move(state));

        RoutineHandle handle(core_, raw->id);
        advance_(*raw);
        reap_();
        return handle;
    }

    /**
     * @brief Stops one routine, destroying its remaining coroutine frames.
     * @param handle The routine to stop.
     * @return True if a running routine was stopped by this call.
     */
    bool stop(const RoutineHandle& handle) { return stop_id_(handle.id()); }

    /**
     * @brief Stops every routine started with the given owner token.
     * @param owner The token passed to start(). Passing nullptr stops only the
     *        routines that were started with no owner.
     * @return How many running routines were stopped.
     */
    size_t stop_owner(const void* owner) {
        size_t stopped = 0;
        for_each_state_([&](detail::RoutineState& s) {
            if (!s.retired && s.owner == owner) {
                request_stop_(s);
                ++stopped;
            }
        });
        reap_();
        return stopped;
    }

    /**
     * @brief Stops every routine this runner owns.
     * @return How many running routines were stopped.
     */
    size_t stop_all() {
        size_t stopped = 0;
        for_each_state_([&](detail::RoutineState& s) {
            if (!s.retired) {
                request_stop_(s);
                ++stopped;
            }
        });
        reap_();
        return stopped;
    }

    /**
     * @brief Checks whether a routine is still running.
     * @param id The routine identity to look up.
     * @return True if the routine has neither finished nor been stopped.
     */
    bool is_running(RoutineId id) const {
        auto it = index_.find(id);
        return it != index_.end() && !it->second->retired;
    }

    /**
     * @brief Resumes every routine whose wait is satisfied, once each.
     *
     * Ignored if called re-entrantly from inside a routine body -- a routine
     * that pumps its own runner would reap states the outer pump is still
     * walking.
     *
     * @param delta_time Scaled seconds since the last tick; drives `seconds()`.
     * @param unscaled_delta_time Unscaled seconds since the last tick; drives
     *        `seconds_realtime()`.
     */
    void tick(float delta_time, float unscaled_delta_time) {
        if (pumping_) {
            logger_.warn("tick() called re-entrantly from a routine body; ignored");
            return;
        }
        pumping_ = true;
        const size_t count = active_.size();
        for (size_t i = 0; i < count; ++i) {
            detail::RoutineState& s = *active_[i];
            if (s.retired) continue;
            if (!ready_(s, delta_time, unscaled_delta_time)) continue;
            advance_(s);
        }
        pumping_ = false;

        for (auto& p : pending_) active_.push_back(std::move(p));
        pending_.clear();
        reap_();
    }

    /**
     * @brief Convenience overload for callers with no separate unscaled clock.
     * @param delta_time Seconds since the last tick, used as both scaled and unscaled.
     */
    void tick(float delta_time) { tick(delta_time, delta_time); }

    /**
     * @brief How many routines are currently running.
     * @return Count of routines that have neither finished nor been stopped.
     */
    size_t active_count() const {
        size_t n = 0;
        for (const auto& s : active_) if (!s->retired) ++n;
        for (const auto& s : pending_) if (!s->retired) ++n;
        return n;
    }

    /**
     * @brief The most recent exception that escaped a routine body.
     *
     * An escaping exception is logged and stops that routine; it is never
     * rethrown out of tick(), because that would take down the frame loop.
     * This is how a caller gets at it afterwards.
     *
     * @return The stored exception, or null if none has occurred since the last clear.
     */
    std::exception_ptr last_exception() const { return last_exception_; }

    /// @brief Clears last_exception().
    void clear_last_exception() { last_exception_ = nullptr; }

private:
    friend class RoutineHandle;

    /// @brief Applies `fn` to every state this runner owns, active and pending alike.
    template<typename F>
    void for_each_state_(F&& fn) {
        for (auto& s : active_) fn(*s);
        for (auto& s : pending_) fn(*s);
    }

    /// @brief Stops the routine with the given id, if it is still running.
    bool stop_id_(RoutineId id) {
        auto it = index_.find(id);
        if (it == index_.end() || it->second->retired) return false;
        request_stop_(*it->second);
        reap_();
        return true;
    }

    /**
     * @brief Marks a routine for removal and tears down what it owns.
     *
     * The frames are destroyed here (top of the stack first, so a child's
     * locals go before its parent's) unless the routine is the one currently
     * executing -- advance_() finishes that case the moment resume() returns.
     */
    void request_stop_(detail::RoutineState& s) {
        s.retired = true;
        release_wait_(s);
        if (!s.executing) destroy_stack_(s);
    }

    /// @brief Destroys the routine's frames from the innermost outwards.
    void destroy_stack_(detail::RoutineState& s) {
        while (!s.stack.empty()) s.stack.pop_back();
    }

    /// @brief Closes a JobHandle this runner allocated for an `on_worker()` body.
    void release_wait_(detail::RoutineState& s) {
        if (s.wait.owns_job && s.wait.job_handle.is_valid()) {
            s.wait.job_handle.close();
        }
        s.wait.owns_job = false;
        s.wait.job_handle = coopa::job::JobHandle();
    }

    /// @brief Erases every retired state. Deferred while a tick is in flight.
    void reap_() {
        if (pumping_) return;
        for (auto it = active_.begin(); it != active_.end(); ) {
            detail::RoutineState& s = **it;
            if (s.retired && !s.executing) {
                destroy_stack_(s);
                index_.erase(s.id);
                it = active_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /**
     * @brief Decides whether a suspended routine may resume this tick, advancing
     *        whatever countdown its wait carries.
     */
    bool ready_(detail::RoutineState& s, float delta_time, float unscaled_delta_time) {
        // Countdowns land a hair below zero from accumulated float error; the
        // epsilon is what makes ten 0.1f ticks satisfy seconds(1.0f) on the
        // tenth rather than the eleventh.
        constexpr float k_epsilon = 1e-5f;
        switch (s.wait.kind) {
            case YieldKind::NextFrame:
                return true;
            case YieldKind::Frames:
                if (s.wait.frames_remaining > 0) --s.wait.frames_remaining;
                return s.wait.frames_remaining == 0;
            case YieldKind::Seconds:
                s.wait.seconds_remaining -= delta_time;
                return s.wait.seconds_remaining <= k_epsilon;
            case YieldKind::SecondsRealtime:
                s.wait.seconds_remaining -= unscaled_delta_time;
                return s.wait.seconds_remaining <= k_epsilon;
            case YieldKind::Until:
                return !s.wait.predicate || s.wait.predicate();
            case YieldKind::While:
                return !s.wait.predicate || !s.wait.predicate();
            case YieldKind::Job:
            case YieldKind::Worker:
                return s.wait.job_handle.is_complete();
            case YieldKind::Nested:
                return true;
        }
        return true;
    }

    /**
     * @brief Resumes the routine until it rests on a real wait or retires.
     *
     * Nested routines are pushed and run within this same call, and a child
     * that completes hands control straight back to its parent in the same
     * tick -- a deliberate departure from Unity, which costs a frame per
     * nesting level.
     */
    void advance_(detail::RoutineState& s) {
        for (;;) {
            if (s.stack.empty()) {
                retire_(s);
                return;
            }

            Routine::handle_type frame = s.stack.back().handle();
            s.executing = true;
            frame.resume();
            s.executing = false;

            // stop() called on this routine from inside its own body: the frame
            // could not be destroyed while it was running, so finish that now.
            if (s.retired) {
                destroy_stack_(s);
                return;
            }

            detail::RoutinePromise& promise = frame.promise();
            if (promise.error) {
                fault_(s, promise.error);
                return;
            }

            if (frame.done()) {
                s.stack.pop_back();
                if (s.stack.empty()) {
                    retire_(s);
                    return;
                }
                continue; // The parent picks up where it yielded, this same tick.
            }

            YieldInstruction yielded = std::move(promise.pending);
            promise.pending = YieldInstruction();

            if (yielded.kind == YieldKind::Nested) {
                std::coroutine_handle<> child = yielded.take_nested();
                if (!child) continue; // `co_yield Routine()` -- nothing to run.
                s.stack.emplace_back(Routine::handle_type::from_address(child.address()));
                continue;             // Unity parity: the child starts immediately.
            }

            release_wait_(s);
            s.wait = std::move(yielded);
            if (s.wait.kind == YieldKind::Worker) arm_worker_(s);
            return;
        }
    }

    /**
     * @brief Submits a freshly yielded `on_worker()` body to the engine and
     *        converts the wait into a plain handle wait.
     *
     * With no engine, no free handle, or no body, the work runs inline right
     * here and the routine resumes on the next tick.
     */
    void arm_worker_(detail::RoutineState& s) {
        if (!s.wait.worker_body) {
            s.wait.kind = YieldKind::NextFrame;
            return;
        }
        coopa::job::JobHandle handle;
        if (jobs_) handle = jobs_->create_handle();
        if (!handle.is_valid()) {
            if (jobs_) logger_.warn("on_worker(): handle pool exhausted, running the body inline");
            s.wait.worker_body();
            s.wait.worker_body = nullptr;
            s.wait.kind = YieldKind::NextFrame;
            return;
        }
        s.wait.job_handle = handle;
        s.wait.owns_job = true;
        jobs_->submit(std::move(s.wait.worker_body), s.wait.job_type, handle,
                      nullptr, 0u, s.wait.job_priority);
        s.wait.worker_body = nullptr;
    }

    /// @brief Logs an escaped exception, stores it, and stops the routine.
    void fault_(detail::RoutineState& s, const std::exception_ptr& error) {
        last_exception_ = error;
        std::string what = "unknown exception";
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        logger_.error("Routine " + std::to_string(s.id) + " aborted: " + what);
        destroy_stack_(s);
        retire_(s);
    }

    /// @brief Marks a routine finished and releases anything it was waiting on.
    void retire_(detail::RoutineState& s) {
        s.retired = true;
        release_wait_(s);
    }

    std::shared_ptr<detail::RunnerCore> core_;  /**< Control block every handle weakly refers to. */
    coopa::job::JobEngine* jobs_ = nullptr;     /**< Non-owning; drives `on_worker()`. */
    coopa::debug::Logger logger_;               /**< Reports escaped exceptions. */

    std::vector<std::unique_ptr<detail::RoutineState>> active_;  /**< Routines the pump walks. */
    std::vector<std::unique_ptr<detail::RoutineState>> pending_; /**< Started mid-tick; merged at the end of it. */
    std::unordered_map<RoutineId, detail::RoutineState*> index_; /**< id -> state, for O(1) handle lookup. */

    RoutineId next_id_ = k_invalid_routine + 1; /**< Monotonic; ids are never reused. */
    bool pumping_ = false;                      /**< True while inside tick(); defers reaping. */
    std::exception_ptr last_exception_;         /**< See last_exception(). */
};

inline bool RoutineHandle::is_running() const {
    auto core = core_.lock();
    return core && core->runner && core->runner->is_running(id_);
}

inline bool RoutineHandle::stop() const {
    auto core = core_.lock();
    if (!core || !core->runner) return false;
    return core->runner->stop_id_(id_);
}

/**
 * @class RoutineScope
 * @brief Move-only RAII owner that stops every routine it started when destroyed.
 *
 * The RoutineRunner analogue of coopa::event::ScopedConnection. Hold one as a
 * member of whatever the routines belong to, and their lifetime is that
 * member's lifetime:
 *
 * @code
 * class Door : public coopa::scene::Component {
 *     coopa::routine::RoutineScope routines_;   // stops on destruction
 *     void start() override { routines_.start(*this, open_slowly()); }
 *     coopa::routine::Routine open_slowly() { ... }
 * };
 * @endcode
 */
class RoutineScope {
public:
    /// @brief Constructs an empty scope.
    RoutineScope() = default;

    /// @brief Stops every routine this scope started.
    ~RoutineScope() { stop_all(); }

    /// @brief Move constructor. Takes over the other scope's routines.
    RoutineScope(RoutineScope&& other) noexcept : handles_(std::move(other.handles_)) {
        other.handles_.clear();
    }

    /// @brief Move assignment. Stops this scope's routines, then takes over the other's.
    RoutineScope& operator=(RoutineScope&& other) noexcept {
        if (this != &other) {
            stop_all();
            handles_ = std::move(other.handles_);
            other.handles_.clear();
        }
        return *this;
    }

    /// @brief Non-copyable: two scopes must not both stop the same routines.
    RoutineScope(const RoutineScope&) = delete;
    /// @brief Non-copyable.
    RoutineScope& operator=(const RoutineScope&) = delete;

    /**
     * @brief Starts a routine on `runner` and keeps it alive only as long as this scope.
     * @param runner The runner to start on.
     * @param r The routine to run.
     * @param owner Optional owner token forwarded to RoutineRunner::start().
     * @return A handle naming the routine.
     */
    RoutineHandle start(RoutineRunner& runner, Routine&& r, const void* owner = nullptr) {
        prune_();
        RoutineHandle handle = runner.start(std::move(r), owner);
        if (handle) handles_.push_back(handle);
        return handle;
    }

    /**
     * @brief Starts a routine on the runner reachable from `owner`'s Scene, owned by `owner`.
     *
     * Defined in coopa/routine/routine_system.h -- include that header to use
     * this overload.
     *
     * @param owner The component the routine belongs to.
     * @param r The routine to run.
     * @return A handle naming the routine, or an empty handle if the component's
     *         Scene has no RoutineSystem registered.
     */
    RoutineHandle start(coopa::scene::Component& owner, Routine&& r);

    /**
     * @brief Stops every routine this scope started.
     * @return How many running routines were stopped.
     */
    size_t stop_all() {
        size_t stopped = 0;
        for (const RoutineHandle& h : handles_) {
            if (h.stop()) ++stopped;
        }
        handles_.clear();
        return stopped;
    }

    /**
     * @brief How many routines this scope started are still running.
     * @return Count of live routines.
     */
    size_t active_count() const {
        size_t n = 0;
        for (const RoutineHandle& h : handles_) if (h.is_running()) ++n;
        return n;
    }

private:
    /// @brief Drops handles whose routines already finished, so a long-lived scope stays bounded.
    void prune_() {
        for (auto it = handles_.begin(); it != handles_.end(); ) {
            it = it->is_running() ? it + 1 : handles_.erase(it);
        }
    }

    std::vector<RoutineHandle> handles_; /**< Every routine started through this scope. */
};

} // namespace routine
} // namespace coopa

#endif // COOPA_ROUTINE_RUNNER_H
