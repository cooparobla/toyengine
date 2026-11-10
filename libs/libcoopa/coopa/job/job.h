/**
 * @file job.h
 * @brief Defines the Job structure representing a task in the work-stealing job system.
 *
 * Optimized for realtime/game-engine use: TaskWrapper is a small-buffer-optimized
 * callable (no heap allocation in the common case) instead of a bare
 * std::function, and dispatches to either a plain `void()` body or a
 * `void(const JobContext&)` body depending on what the caller supplied.
 *
 * A Job carries no dependency array of its own: an unmet dependency list lives
 * in a PendingNode in dependency_graph.h, which resolves dependencies
 * event-driven and with no inline-count limit. That keeps Job itself small for
 * the (overwhelmingly common) zero-dependency fast path.
 */

#ifndef COOPA_JOB_JOB_H
#define COOPA_JOB_JOB_H

#include <cstdint>
#include <cstring>    // For std::memcpy
#include <functional> // For std::function (TaskWrapper's overflow path)
#include <new>        // For placement new
#include <type_traits>
#include <utility>    // For std::move

#include <coopa/job/context.h>
#include <coopa/job/handle.h>
#include <coopa/job/platform.h>

namespace coopa {
namespace job {

/**
 * @class TaskWrapper
 * @brief A small-buffer-optimized callable that avoids heap allocation for
 *        callables up to k_task_buffer_size bytes.
 *
 * Accepts any callable invocable as either `void()` or `void(const
 * JobContext&)` -- resolved at construction time via `if constexpr`, so a
 * plain capture-only lambda pays nothing for the context it never asked for,
 * while a job body that wants its worker index or cancellation flag can ask
 * for `const JobContext&` and receive it for free.
 *
 * Uses placement new into an inline buffer. For callables that exceed the
 * buffer size, falls back to std::function<void(const JobContext&)> stored in
 * the buffer (which may itself heap-allocate, but this path is rare in practice).
 */
class TaskWrapper {
    /// @brief Size of the inline callable storage buffer in bytes.
    static constexpr std::size_t k_task_buffer_size = 48;

    using FallbackFn = std::function<void(const JobContext&)>;

    /// @brief Type-erased function pointer for invoking the stored callable.
    using InvokeFn = void(*)(void*, const JobContext&);
    /// @brief Type-erased function pointer for destroying the stored callable.
    using DestroyFn = void(*)(void*);
    /// @brief Type-erased function pointer for move-constructing from one buffer to another.
    using MoveFn = void(*)(void* dst, void* src);

    /**
     * @brief Typed invoker template: calls F() or F(ctx), whichever it supports.
     * @tparam F The callable type.
     */
    template<typename F>
    static void typed_invoke(void* ptr, const JobContext& ctx) {
        F& f = *static_cast<F*>(ptr);
        if constexpr (std::is_invocable_v<F&, const JobContext&>) {
            f(ctx);
        } else {
            f();
        }
    }

    template<typename F>
    static void typed_destroy(void* ptr) {
        static_cast<F*>(ptr)->~F();
    }

    template<typename F>
    static void typed_move(void* dst, void* src) {
        new (dst) F(std::move(*static_cast<F*>(src)));
        static_cast<F*>(src)->~F();
    }

public:
    /// @brief Default constructor. Creates an empty (no-op) TaskWrapper.
    TaskWrapper() : invoke_(nullptr), destroy_(nullptr), move_(nullptr) {}

    /**
     * @brief Constructs a TaskWrapper from any callable invocable as `void()`
     *        or `void(const JobContext&)`.
     * @tparam F The callable type.
     * @param f The callable to wrap.
     */
    template<typename F,
             typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, TaskWrapper>>>
    TaskWrapper(F&& f) {
        using DecayedF = std::decay_t<F>;

        if constexpr (sizeof(DecayedF) <= k_task_buffer_size &&
                      alignof(DecayedF) <= alignof(std::max_align_t) &&
                      std::is_nothrow_move_constructible_v<DecayedF>) {
            // Fast path: fits in inline buffer.
            new (buffer_) DecayedF(std::forward<F>(f));
            invoke_ = &typed_invoke<DecayedF>;
            destroy_ = &typed_destroy<DecayedF>;
            move_ = &typed_move<DecayedF>;
        } else {
            // Slow path: wrap in std::function (rare for game jobs).
            static_assert(sizeof(FallbackFn) <= k_task_buffer_size,
                          "k_task_buffer_size must be large enough for the fallback std::function");
            new (buffer_) FallbackFn(
                [g = std::forward<F>(f)](const JobContext& ctx) mutable {
                    if constexpr (std::is_invocable_v<DecayedF&, const JobContext&>) {
                        g(ctx);
                    } else {
                        (void)ctx;
                        g();
                    }
                });
            invoke_ = &typed_invoke<FallbackFn>;
            destroy_ = &typed_destroy<FallbackFn>;
            move_ = &typed_move<FallbackFn>;
        }
    }

    /// @brief Destructor. Destroys the stored callable if present.
    ~TaskWrapper() {
        if (destroy_) destroy_(buffer_);
    }

    /// @brief Move constructor. Transfers ownership of the callable.
    TaskWrapper(TaskWrapper&& other) noexcept
        : invoke_(other.invoke_), destroy_(other.destroy_), move_(other.move_)
    {
        if (move_) move_(buffer_, other.buffer_);
        other.invoke_ = nullptr;
        other.destroy_ = nullptr;
        other.move_ = nullptr;
    }

    /// @brief Move assignment. Destroys current callable and takes ownership.
    TaskWrapper& operator=(TaskWrapper&& other) noexcept {
        if (this != &other) {
            if (destroy_) destroy_(buffer_);
            invoke_ = other.invoke_;
            destroy_ = other.destroy_;
            move_ = other.move_;
            if (move_) move_(buffer_, other.buffer_);
            other.invoke_ = nullptr;
            other.destroy_ = nullptr;
            other.move_ = nullptr;
        }
        return *this;
    }

    /// @brief Non-copyable.
    TaskWrapper(const TaskWrapper&) = delete;
    /// @brief Non-copyable.
    TaskWrapper& operator=(const TaskWrapper&) = delete;

    /**
     * @brief Invokes the stored callable.
     * @param ctx Context handed to callables that asked for one.
     * @note No-op if the wrapper is empty.
     */
    void operator()(const JobContext& ctx) {
        if (invoke_) invoke_(buffer_, ctx);
    }

    /**
     * @brief Checks if the wrapper contains a valid callable.
     * @return True if a callable is stored.
     */
    explicit operator bool() const { return invoke_ != nullptr; }

private:
    alignas(std::max_align_t) char buffer_[k_task_buffer_size]; /**< Inline callable storage. */
    InvokeFn invoke_;    /**< Type-erased invoke function pointer. */
    DestroyFn destroy_;  /**< Type-erased destroy function pointer. */
    MoveFn move_;        /**< Type-erased move function pointer. */
};

/**
 * @struct Job
 * @brief Represents a single unit of work to be executed by a worker thread.
 *
 * Cache-friendly and allocation-free to construct: TaskWrapper avoids heap
 * allocation for typical lambda captures. A job's dependency list, if any, is
 * not stored here -- see dependency_graph.h, which owns dependency-having
 * jobs until they become runnable and hands them back as a plain Job.
 */
struct Job {
    TaskWrapper task;    /**< The work callback to perform. */
    JobHandle handle;    /**< The handle this job contributes to (decremented upon completion). */
    JobType type = 0;    /**< The category/type of this job. */
    Priority priority = Priority::Normal; /**< Scheduling priority within a worker's own deque. */

    /// @brief Default constructor. Produces an empty, non-executable job.
    Job() = default;

    /**
     * @brief Constructs a Job with a callback, type, handle, and priority.
     * @tparam F Callable type (`void()` or `void(const JobContext&)`).
     * @param fn Task callback.
     * @param t Job category type.
     * @param h Output completion handle.
     * @param p Scheduling priority.
     */
    template<typename F>
    Job(F&& fn, JobType t, JobHandle h, Priority p = Priority::Normal)
        : task(std::forward<F>(fn)), handle(h), type(t), priority(p) {}

    /// @brief Non-copyable (TaskWrapper is non-copyable).
    Job(const Job&) = delete;
    /// @brief Non-copyable.
    Job& operator=(const Job&) = delete;

    /// @brief Move constructor.
    Job(Job&& other) noexcept = default;
    /// @brief Move assignment.
    Job& operator=(Job&& other) noexcept = default;
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_JOB_H
