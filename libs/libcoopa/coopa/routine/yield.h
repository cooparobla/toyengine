/**
 * @file yield.h
 * @brief The yield vocabulary a Routine suspends on, and the single internal
 *        instruction the RoutineRunner reads.
 *
 * Every factory in this header returns a YieldInstruction, which is what a
 * routine body hands back through `co_yield`. One instruction exists per
 * *suspended routine* (it lives on that routine's coroutine promise), never
 * one per frame, so the type is written for clarity rather than for size.
 *
 * The nested-routine case is carried as a type-erased `std::coroutine_handle<>`
 * so this header never has to know about Routine -- routine.h includes this
 * one, not the other way around.
 */

#ifndef COOPA_ROUTINE_YIELD_H
#define COOPA_ROUTINE_YIELD_H

#include <coroutine>
#include <cstdint>
#include <functional>
#include <utility>

#include <coopa/job/context.h>
#include <coopa/job/handle.h>

namespace coopa {
namespace routine {

/**
 * @brief What a suspended routine is waiting for.
 *
 * The runner switches on this every tick to decide whether a routine may
 * resume. `Nested` never rests here: the runner converts it into a push onto
 * the routine's call stack the instant it is yielded.
 */
enum class YieldKind : uint8_t {
    NextFrame,       ///< Resume on the next tick (Unity's `yield return null`).
    Frames,          ///< Resume after `frames_remaining` ticks.
    Seconds,         ///< Resume once `seconds_remaining` of *scaled* delta has elapsed.
    SecondsRealtime, ///< Resume once `seconds_remaining` of *unscaled* delta has elapsed.
    Until,           ///< Resume once `predicate()` returns true.
    While,           ///< Resume once `predicate()` returns false.
    Job,             ///< Resume once `job_handle` completes.
    Worker,          ///< Submit `worker_body` to the JobEngine, then resume once it completes.
    Nested,          ///< Run `nested` to completion first, then resume the yielding routine.
};

/**
 * @struct YieldInstruction
 * @brief One suspension request, produced by the factories below and consumed
 *        by RoutineRunner.
 *
 * Move-only, and owns two things it must clean up if it is destroyed before
 * the runner takes them: a nested coroutine frame, and a JobHandle the runner
 * itself allocated for a `Worker` instruction (`owns_job`).
 */
struct YieldInstruction {
    YieldKind kind = YieldKind::NextFrame;          /**< Which wait this is. */
    float     seconds_remaining = 0.0f;             /**< Countdown for Seconds/SecondsRealtime. */
    uint32_t  frames_remaining  = 0;                /**< Countdown for Frames. */
    std::function<bool()> predicate;                /**< Condition for Until/While. */
    std::function<void()> worker_body;              /**< Body submitted to the engine for Worker. */
    coopa::job::JobHandle job_handle;               /**< Awaited handle for Job, and for an armed Worker. */
    coopa::job::JobType   job_type = 0;             /**< Job category used when submitting a Worker body. */
    coopa::job::Priority  job_priority = coopa::job::Priority::Normal; /**< Priority used for a Worker body. */
    bool owns_job = false;                          /**< True when the runner allocated job_handle and must close it. */
    std::coroutine_handle<> nested{};               /**< Owned child frame for Nested, until the runner takes it. */

    /// @brief Default-constructs a plain "resume on the next tick" instruction.
    YieldInstruction() = default;

    /// @brief Move constructor. Transfers ownership of the nested frame.
    YieldInstruction(YieldInstruction&& other) noexcept
        : kind(other.kind),
          seconds_remaining(other.seconds_remaining),
          frames_remaining(other.frames_remaining),
          predicate(std::move(other.predicate)),
          worker_body(std::move(other.worker_body)),
          job_handle(other.job_handle),
          job_type(other.job_type),
          job_priority(other.job_priority),
          owns_job(other.owns_job),
          nested(other.nested)
    {
        other.nested = {};
        other.owns_job = false;
    }

    /// @brief Move assignment. Destroys any nested frame this instruction still owns.
    YieldInstruction& operator=(YieldInstruction&& other) noexcept {
        if (this != &other) {
            if (nested) nested.destroy();
            kind = other.kind;
            seconds_remaining = other.seconds_remaining;
            frames_remaining = other.frames_remaining;
            predicate = std::move(other.predicate);
            worker_body = std::move(other.worker_body);
            job_handle = other.job_handle;
            job_type = other.job_type;
            job_priority = other.job_priority;
            owns_job = other.owns_job;
            nested = other.nested;
            other.nested = {};
            other.owns_job = false;
        }
        return *this;
    }

    /// @brief Non-copyable: it owns a coroutine frame.
    YieldInstruction(const YieldInstruction&) = delete;
    /// @brief Non-copyable.
    YieldInstruction& operator=(const YieldInstruction&) = delete;

    /// @brief Destroys a nested frame the runner never took ownership of.
    ~YieldInstruction() {
        if (nested) nested.destroy();
    }

    /**
     * @brief Hands the nested frame to the caller, who becomes responsible for destroying it.
     * @return The child coroutine handle, or an empty handle if there is none.
     */
    std::coroutine_handle<> take_nested() {
        std::coroutine_handle<> h = nested;
        nested = {};
        return h;
    }
};

/**
 * @brief Resumes the routine on the next tick -- Unity's `yield return null`.
 * @return The instruction to `co_yield`.
 */
inline YieldInstruction next_frame() {
    YieldInstruction y;
    y.kind = YieldKind::NextFrame;
    return y;
}

/**
 * @brief Resumes the routine after `count` ticks. `frames(1)` is `next_frame()`.
 * @param count Number of ticks to skip. Zero resumes on the very next tick.
 * @return The instruction to `co_yield`.
 */
inline YieldInstruction frames(uint32_t count) {
    YieldInstruction y;
    y.kind = YieldKind::Frames;
    y.frames_remaining = count;
    return y;
}

/**
 * @brief Resumes once `duration` seconds of *scaled* time have passed -- Unity's `WaitForSeconds`.
 *
 * Scaled time is whatever the caller passes as RoutineRunner::tick()'s first
 * argument; RoutineSystem multiplies the frame delta by its time scale there,
 * so a paused game (time scale 0) never advances this countdown.
 *
 * @param duration Seconds to wait.
 * @return The instruction to `co_yield`.
 */
inline YieldInstruction seconds(float duration) {
    YieldInstruction y;
    y.kind = YieldKind::Seconds;
    y.seconds_remaining = duration;
    return y;
}

/**
 * @brief Resumes once `duration` seconds of *unscaled* time have passed -- Unity's
 *        `WaitForSecondsRealtime`. Unaffected by the time scale.
 * @param duration Seconds to wait.
 * @return The instruction to `co_yield`.
 */
inline YieldInstruction seconds_realtime(float duration) {
    YieldInstruction y;
    y.kind = YieldKind::SecondsRealtime;
    y.seconds_remaining = duration;
    return y;
}

/**
 * @brief Resumes on the first tick where `pred()` returns true -- Unity's `WaitUntil`.
 *
 * The predicate is evaluated once per tick on the thread driving tick(), so it
 * may read anything that thread may read.
 *
 * @tparam P Callable returning something contextually convertible to bool.
 * @param pred The condition to poll.
 * @return The instruction to `co_yield`.
 */
template<typename P>
YieldInstruction wait_until(P&& pred) {
    YieldInstruction y;
    y.kind = YieldKind::Until;
    y.predicate = std::forward<P>(pred);
    return y;
}

/**
 * @brief Resumes on the first tick where `pred()` returns false -- Unity's `WaitWhile`.
 * @tparam P Callable returning something contextually convertible to bool.
 * @param pred The condition to poll.
 * @return The instruction to `co_yield`.
 */
template<typename P>
YieldInstruction wait_while(P&& pred) {
    YieldInstruction y;
    y.kind = YieldKind::While;
    y.predicate = std::forward<P>(pred);
    return y;
}

/**
 * @brief Resumes once a coopa::job::JobHandle completes, without blocking a thread.
 *
 * The routine occupies no worker while it waits; the runner polls
 * `JobHandle::is_complete()` once per tick, which is a single relaxed atomic
 * load. Ownership of the handle stays with whoever created it -- the runner
 * never calls `close()` on it, per the close-exactly-once contract in
 * coopa/job/handle.h. An invalid handle reports complete immediately.
 *
 * @param handle The handle to await.
 * @return The instruction to `co_yield`.
 */
inline YieldInstruction wait_for(const coopa::job::JobHandle& handle) {
    YieldInstruction y;
    y.kind = YieldKind::Job;
    y.job_handle = handle;
    y.owns_job = false;
    return y;
}

/**
 * @brief Runs `body` on the JobEngine and resumes the routine, back on the
 *        tick thread, once it finishes.
 *
 * This is the piece Unity has no equivalent for: the heavy half of a routine
 * runs across the work-stealing engine while the routine itself is suspended,
 * and the code after the `co_yield` is back on the tick thread and free to
 * touch the Scene again.
 *
 * The runner allocates and closes the handle for this job itself. With no
 * engine installed on the runner, `body` runs inline on the tick thread
 * instead and the routine resumes on the next tick -- the same graceful
 * degradation a null `FrameContext::jobs` already gives every scene system.
 *
 * @tparam F Callable invocable as `void()`.
 * @param body The work to run on a worker thread.
 * @param type Job category, for engine thread dedication and diagnostics.
 * @param priority Scheduling priority within a worker's deque.
 * @return The instruction to `co_yield`.
 */
template<typename F>
YieldInstruction on_worker(F&& body,
                           coopa::job::JobType type = 0,
                           coopa::job::Priority priority = coopa::job::Priority::Normal) {
    YieldInstruction y;
    y.kind = YieldKind::Worker;
    y.worker_body = std::forward<F>(body);
    y.job_type = type;
    y.job_priority = priority;
    return y;
}

} // namespace routine
} // namespace coopa

#endif // COOPA_ROUTINE_YIELD_H
