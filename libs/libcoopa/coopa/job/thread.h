/**
 * @file thread.h
 * @brief Defines the worker Thread class wrapping std::thread and lifecycle states.
 *
 * Provides a managed thread wrapper with signal-stop semantics and lifecycle
 * logging. The worker loop callback receives the thread ID and stop flag;
 * synchronization (CV/mutex) is managed externally by the JobEngine.
 */

#ifndef COOPA_JOB_THREAD_H
#define COOPA_JOB_THREAD_H

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <string>

#include <coopa/debug/logger.h>

namespace coopa {
namespace job {

/**
 * @class Thread
 * @brief Managed worker thread instance in the job system pool.
 *
 * Each Thread wraps a std::thread and provides:
 * - Atomic stop flag for cooperative cancellation.
 * - Per-thread mutex and condition variable, passed to the loop callback for
 *   callbacks that want to park on them. JobEngine's own worker loop parks on
 *   the engine-wide pair instead, so it ignores both.
 * - Lifecycle logging on construction and destruction.
 *
 * Thread is non-copyable and non-movable.
 */
class Thread {
public:
    /**
     * @brief Constructs a worker Thread and immediately starts execution.
     * @param id The unique integer ID of this worker thread.
     * @param thread_loop_func The loop callback (passed thread id, stop flag, mutex, cv).
     * @param logger Pointer to the shared logger instance.
     */
    Thread(unsigned int id,
           std::function<void(unsigned int, std::atomic<bool>&, std::mutex&, std::condition_variable&)> thread_loop_func,
           coopa::debug::Logger* logger)
        : id_(id),
          stop_flag_(false),
          thread_loop_func_(std::move(thread_loop_func)),
          logger_(logger)
    {
        worker_thread_ = std::thread(&Thread::run, this);
        logger_->info("Thread " + std::to_string(id_) + " started.");
    }

    /**
     * @brief Destructor. Blocks until the thread completes joining.
     */
    ~Thread() {
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        logger_->info("Thread " + std::to_string(id_) + " destroyed.");
    }

    /// @brief Non-copyable.
    Thread(const Thread&) = delete;

    /// @brief Non-copyable.
    Thread& operator=(const Thread&) = delete;

    /// @brief Non-movable.
    Thread(Thread&&) = delete;

    /// @brief Non-movable.
    Thread& operator=(Thread&&) = delete;

    /**
     * @brief Signals the thread to stop and notifies its condition variable.
     */
    void signal_stop() {
        stop_flag_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    /**
     * @brief Checks if the underlying thread is joinable.
     * @return True if joinable.
     */
    bool joinable() const {
        return worker_thread_.joinable();
    }

    /**
     * @brief Blocks until this worker thread completes execution.
     */
    void join() {
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }

    /**
     * @brief Gets the worker thread's internal ID.
     * @return Thread ID.
     */
    unsigned int get_id() const { return id_; }

private:
    /**
     * @brief Thread execution entry point that invokes the thread_loop_func_ callback.
     */
    void run() {
        if (thread_loop_func_) {
            thread_loop_func_(id_, stop_flag_, mutex_, cv_);
        }
    }

    unsigned int id_;           /**< Unique worker thread ID. */
    std::thread worker_thread_; /**< Underlying std::thread. */
    std::atomic<bool> stop_flag_; /**< Cooperative stop signal. */
    std::mutex mutex_;          /**< Per-thread mutex passed to the loop callback (JobEngine's loop ignores it and sleeps on its own shared CV). */
    std::condition_variable cv_; /**< Per-thread CV passed to the loop callback and notified by signal_stop() (unused by JobEngine's loop). */
    std::function<void(unsigned int, std::atomic<bool>&, std::mutex&, std::condition_variable&)> thread_loop_func_;
    coopa::debug::Logger* logger_; /**< Pointer to the shared logger instance (non-owning). */
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_THREAD_H