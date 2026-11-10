/**
 * @file routine.h
 * @brief The Routine coroutine type -- a Unity-style coroutine written as
 *        straight-line C++20 code.
 *
 * A function returning `Routine` is a coroutine. It suspends by handing back a
 * YieldInstruction (see yield.h) through `co_yield`, and a RoutineRunner (see
 * runner.h) decides each tick which suspended routines may resume:
 *
 * @code
 * coopa::routine::Routine open_door(Door& door) {
 *     door.play_sound();
 *     co_yield coopa::routine::seconds(0.2f);
 *     for (float t = 0.0f; t < 1.0f; t += 0.016f) {
 *         door.set_angle(t * 90.0f);
 *         co_yield coopa::routine::next_frame();
 *     }
 *     co_yield coopa::routine::wait_until([&door] { return door.player_inside(); });
 *     door.close();
 * }
 * @endcode
 *
 * A Routine owns its coroutine frame and destroys it on destruction, so an
 * abandoned or stopped routine runs its locals' destructors -- RAII inside a
 * routine body is sound.
 */

#ifndef COOPA_ROUTINE_ROUTINE_H
#define COOPA_ROUTINE_ROUTINE_H

#include <coroutine>
#include <exception>
#include <utility>

#include <coopa/job/handle.h>
#include <coopa/routine/yield.h>

namespace coopa {
namespace routine {

class Routine;

namespace detail {

/**
 * @struct RoutinePromise
 * @brief The coroutine promise behind Routine. Holds whatever the body last
 *        yielded, plus any exception that escaped it.
 *
 * Defined ahead of Routine so `std::coroutine_handle<RoutinePromise>` is only
 * ever instantiated against a complete type; the three members that need
 * Routine itself are defined below the class.
 */
struct RoutinePromise {
    YieldInstruction   pending;  /**< What the body yielded at its last suspension point. */
    std::exception_ptr error;    /**< The exception that escaped the body, or null. */

    /// @brief Builds the Routine handed back to whoever called the coroutine.
    Routine get_return_object();

    /**
     * @brief Suspends before the first statement of the body.
     *
     * The body therefore does not run at construction, which keeps a Routine
     * safe to move around before it is started. RoutineRunner::start() does
     * the first resume immediately, reproducing Unity's "StartCoroutine runs
     * the body synchronously up to the first `yield return`".
     */
    std::suspend_always initial_suspend() const noexcept { return {}; }

    /// @brief Suspends at the end, so the runner can observe done() and destroy the frame itself.
    std::suspend_always final_suspend() const noexcept { return {}; }

    /// @brief A routine body produces no value.
    void return_void() const noexcept {}

    /// @brief Captures an exception that escaped the body; the runner logs it and stops the routine.
    void unhandled_exception() noexcept { error = std::current_exception(); }

    /**
     * @brief Handles `co_yield` of any instruction from yield.h.
     * @param instruction What to wait for.
     * @return Always suspends.
     */
    std::suspend_always yield_value(YieldInstruction&& instruction) noexcept {
        pending = std::move(instruction);
        return {};
    }

    /**
     * @brief Handles `co_yield other_routine()` -- Unity's `yield return StartCoroutine(x)`.
     *
     * The child starts immediately and runs to completion before the yielding
     * routine resumes. Ownership of the child's frame passes to this promise
     * and then to the runner.
     *
     * @param child The nested routine to run.
     * @return Always suspends.
     */
    std::suspend_always yield_value(Routine&& child) noexcept;

    /**
     * @brief Handles `co_yield some_job_handle` -- sugar for `co_yield wait_for(h)`,
     *        so `co_yield engine.parallel_for(...)` reads naturally.
     * @param handle The handle to await. Not closed by the runner.
     * @return Always suspends.
     */
    std::suspend_always yield_value(const coopa::job::JobHandle& handle) noexcept;
};

} // namespace detail

/**
 * @class Routine
 * @brief A move-only owner of one coroutine frame -- the return type of every
 *        routine body.
 *
 * Construct one by calling a function whose return type is `Routine` and whose
 * body contains a `co_yield` or `co_return`, then hand it to
 * RoutineRunner::start(). A Routine that is destroyed without ever being
 * started simply destroys its frame without running the body.
 */
class Routine {
public:
    /// @brief The promise type the compiler drives this coroutine through.
    using promise_type = detail::RoutinePromise;
    /// @brief The typed coroutine handle for this routine's frame.
    using handle_type = std::coroutine_handle<promise_type>;

    /// @brief Constructs an empty Routine owning no frame.
    Routine() = default;

    /**
     * @brief Adopts an existing coroutine frame.
     * @param handle The frame to take ownership of.
     */
    explicit Routine(handle_type handle) : handle_(handle) {}

    /// @brief Move constructor. Transfers frame ownership.
    Routine(Routine&& other) noexcept : handle_(other.handle_) { other.handle_ = {}; }

    /// @brief Move assignment. Destroys any frame currently owned.
    Routine& operator=(Routine&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = {};
        }
        return *this;
    }

    /// @brief Non-copyable: a coroutine frame has exactly one owner.
    Routine(const Routine&) = delete;
    /// @brief Non-copyable.
    Routine& operator=(const Routine&) = delete;

    /// @brief Destroys the owned frame, running every live local's destructor.
    ~Routine() {
        if (handle_) handle_.destroy();
    }

    /**
     * @brief Checks whether this Routine owns a frame.
     * @return True if a frame is owned.
     */
    bool valid() const { return handle_ != nullptr; }

    /// @brief Same as valid().
    explicit operator bool() const { return valid(); }

    /**
     * @brief Checks whether the body has run to completion.
     * @return True if there is no frame, or the body has finished.
     */
    bool is_done() const { return !handle_ || handle_.done(); }

    /**
     * @brief The owned coroutine handle.
     * @return The handle, which is empty if this Routine owns no frame.
     */
    handle_type handle() const { return handle_; }

    /**
     * @brief The coroutine's promise.
     * @return A reference to the promise. Undefined if no frame is owned.
     */
    promise_type& promise() const { return handle_.promise(); }

    /**
     * @brief Gives up ownership of the frame without destroying it.
     * @return The handle the caller is now responsible for destroying.
     */
    handle_type release() {
        handle_type h = handle_;
        handle_ = {};
        return h;
    }

private:
    handle_type handle_{}; /**< The owned frame, or empty. */
};

namespace detail {

inline Routine RoutinePromise::get_return_object() {
    return Routine(Routine::handle_type::from_promise(*this));
}

inline std::suspend_always RoutinePromise::yield_value(Routine&& child) noexcept {
    YieldInstruction instruction;
    instruction.kind = YieldKind::Nested;
    instruction.nested = child.release();
    pending = std::move(instruction);
    return {};
}

inline std::suspend_always RoutinePromise::yield_value(const coopa::job::JobHandle& handle) noexcept {
    pending = wait_for(handle);
    return {};
}

} // namespace detail

} // namespace routine
} // namespace coopa

#endif // COOPA_ROUTINE_ROUTINE_H
