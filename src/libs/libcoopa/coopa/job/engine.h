/**
 * @file engine.h
 * @brief Core job engine: a multi-threaded work-stealing job scheduler.
 *
 * Optimized for realtime/game-engine use with:
 * - Lock-free Chase-Lev work-stealing deques, one per priority per worker
 * - Generation-tagged, individually-reclaimed handles (see handle.h) --
 *   handles survive begin_frame()/end_frame() and may be shared by any number
 *   of long-lived subsystems on one engine
 * - Event-driven, unbounded-fan-in dependency resolution (see dependency_graph.h)
 *   instead of an O(N) global-mutex scan
 * - A submitting worker pushes straight to its own deque (continuation-stealing
 *   style, no lock); every other submitter uses a global MPMC queue
 * - Randomized work-stealing victim order (no thread-0 convoying)
 * - A worker's sleep predicate counts only jobs actually sitting in a deque or
 *   queue, so jobs parked on unmet dependencies do not keep workers spinning
 * - wait_for() run from a worker thread participates as that real worker (not a
 *   steal-only guest), so a job may safely wait on another job without deadlock
 *
 * API notes: JobType/Priority live in coopa::job (see context.h); submit()
 * accepts an unbounded dependency count; and every handle returned by
 * create_handle()/submit_jobs() must eventually be closed (see handle.h's
 * JobHandle::close() / ScopedJobHandle) to recycle its slot.
 */

#ifndef COOPA_JOB_ENGINE_H
#define COOPA_JOB_ENGINE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/job/collections/queue.h>
#include <coopa/job/collections/work_stealing_deque.h>
#include <coopa/job/context.h>
#include <coopa/job/dependency_graph.h>
#include <coopa/job/handle.h>
#include <coopa/job/job.h>
#include <coopa/job/platform.h>
#include <coopa/job/thread.h>

namespace coopa {
namespace job {

/// @brief Number of Priority levels (High/Normal/Low) -- used to size per-worker/global queue arrays.
inline constexpr size_t k_priority_count = static_cast<size_t>(Priority::Count);

/**
 * @class JobEngine
 * @brief Processes tasks in parallel across worker threads, utilizing lock-free work-stealing.
 *
 * Non-copyable and non-movable (it owns live OS threads and a mutex/condition
 * variable, neither of which the standard library permits to move). Hold it in
 * a `std::unique_ptr<JobEngine>` wherever it needs to live inside a container --
 * the same pattern libcoopa already uses for other long-lived owned resources
 * (e.g. SceneManager's `std::unique_ptr<Scene>`).
 */
class JobEngine {
public:

#ifdef COOPA_JOB_DIAGNOSTICS
    /// @brief Snapshot of job counts for diagnostics.
    struct Snapshot {
        int total_pending = 0;                              /**< Total pending jobs. */
        std::array<int, k_max_job_types> type_counts = {};  /**< Per-type counts. */
    };

    /// @brief Snapshot of a single worker thread's state for profiling.
    struct ThreadDiagnostics {
        unsigned int thread_id;     /**< Worker thread index. */
        size_t queue_depth;         /**< Approximate items across this thread's priority deques. */
        std::string dedication;     /**< "General" or a comma-separated list of dedicated JobTypes. */
    };
#endif // COOPA_JOB_DIAGNOSTICS

    /**
     * @brief Constructs the JobEngine and spawns its worker threads.
     * @param num_threads Number of worker threads. Defaults to hardware concurrency.
     * @param deque_capacity_per_priority Fixed capacity of each per-worker,
     *        per-priority deque. A full deque is not an error -- submissions
     *        that would overflow it fall back to the global queue.
     * @param handle_pool_capacity Maximum number of concurrently in-flight
     *        (allocated-and-not-yet-closed) JobHandles.
     */
    explicit JobEngine(unsigned int num_threads = std::thread::hardware_concurrency(),
                        uint32_t deque_capacity_per_priority = k_default_job_pool_capacity / 4,
                        uint32_t handle_pool_capacity = k_default_job_pool_capacity)
        : logger_(new coopa::debug::Logger("JobEngine")),
          num_threads_(num_threads > 0 ? num_threads : 1),
          counter_pool_(handle_pool_capacity),
          dep_graph_([this](Job&& j) { dispatch_job_(std::move(j)); })
    {
        logger_->info("JobEngine setup with " + std::to_string(num_threads_) + " threads");

        for (auto& d : thread_dedications_) d.store(UINT32_MAX, std::memory_order_relaxed);
#ifdef COOPA_JOB_DIAGNOSTICS
        for (auto& c : job_type_counts_) c.store(0, std::memory_order_relaxed);
#endif

        per_thread_deques_.resize(num_threads_);
        for (unsigned int i = 0; i < num_threads_; ++i) {
            for (auto& dq : per_thread_deques_[i]) {
                dq = std::make_unique<WorkStealingDeque<Job>>(deque_capacity_per_priority);
            }
        }

        for (unsigned int i = 0; i < num_threads_; ++i) {
            worker_threads_.emplace_back(std::make_unique<coopa::job::Thread>(
                i,
                [this](unsigned int thread_id, std::atomic<bool>& stop_flag, std::mutex& /*mtx*/, std::condition_variable& /*cv*/) {
                    this->worker_thread_loop(thread_id, stop_flag);
                },
                logger_
            ));
        }
    }

    /// @brief Destructor. Shuts down the job system, joins all worker threads, and reports leaks.
    ~JobEngine() {
        shutdown();
        delete logger_;
    }

    /// @brief Non-copyable, non-movable -- see class doc.
    JobEngine(const JobEngine&) = delete;
    JobEngine& operator=(const JobEngine&) = delete;
    JobEngine(JobEngine&&) = delete;
    JobEngine& operator=(JobEngine&&) = delete;

    // --- Frame lifecycle (diagnostics only -- handles are safely reused
    //     without a frame boundary; see handle.h's class doc) ---

    /// @brief Resets per-frame diagnostic counters. No-op when COOPA_JOB_DIAGNOSTICS is off.
    void begin_frame() {
#ifdef COOPA_JOB_DIAGNOSTICS
        for (auto& c : job_type_counts_) c.store(0, std::memory_order_relaxed);
#endif
    }

    /// @brief Reserved for future per-frame finalization (e.g. profiling snapshots).
    void end_frame() {}

    // --- Handle lifecycle ---

    /**
     * @brief Allocates a new JobHandle. Must eventually be closed via
     *        JobHandle::close() (or wrapped in a ScopedJobHandle) once the
     *        caller is done with it, so its slot can be recycled.
     * @return A valid handle, or an invalid one if the handle pool is exhausted.
     */
    JobHandle create_handle() {
        auto alloc = counter_pool_.allocate();
        if (alloc.index == k_invalid_handle_index) {
            logger_->warn("JobEngine: handle pool exhausted");
        }
        return JobHandle(&counter_pool_, alloc.index, alloc.generation);
    }

    /// @brief Convenience equivalent to `handle.close()`.
    void close_handle(const JobHandle& handle) { handle.close(); }

    /**
     * @brief Resolves completion bookkeeping for a job that was executed
     *        outside the normal dispatch loop (e.g. JobScheduler's
     *        main-thread jobs, run directly by JobScheduler::execute_main_thread_jobs()
     *        rather than via try_execute_a_job()).
     *
     * Mirrors what execute_job_() does after invoking a dispatched job's
     * task: decrements the handle's job counter, and if that drives it to
     * zero, wakes any dependents parked on it and attempts a reclaim.
     */
    void complete_external_job(const JobHandle& handle) {
        if (handle.complete_job_and_check_zero_()) {
            dep_graph_.harvest_and_fire(handle);
            handle.try_reclaim_after_completion_();
        }
    }

    /// @brief Cancels every job still queued (not yet running) against this handle.
    void cancel(const JobHandle& handle) {
        if (handle.is_valid()) counter_pool_.cancel(handle.index());
    }

    /// @brief Direct access to the counter pool (diagnostics / advanced use).
    /// Provided for consumers and leak assertions; not used by the engine itself.
    CounterPool& get_counter_pool() { return counter_pool_; }

    // --- Thread dedication ---

    /**
     * @brief Dedicates a job type to a specific worker thread.
     *
     * Jobs of this type always land in a dedicated per-type queue that only
     * `thread_index` ever pulls from: a hard guarantee, not a steal-able hint.
     *
     * Provided for consumers; libcoopa itself does not dedicate any thread.
     */
    void dedicate_thread_to_job_type(JobType type, unsigned int thread_index) {
        if (thread_index >= num_threads_) {
            logger_->warn("Attempted to dedicate invalid thread index: " + std::to_string(thread_index));
            return;
        }
        if (type >= k_max_job_types) {
            logger_->warn("Job type " + std::to_string(type) + " exceeds k_max_job_types (" + std::to_string(k_max_job_types) + ")");
            return;
        }
        std::lock_guard<std::mutex> lock(dedication_mutex_);
        thread_dedications_[type].store(thread_index, std::memory_order_release);
        any_dedications_.store(true, std::memory_order_release);
        logger_->info("Thread " + std::to_string(thread_index) + " dedicated to type " + std::to_string(type));
    }

    // --- Worker identity ---

    /// @brief This engine's worker index for the calling thread, or k_main_thread_index.
    uint32_t worker_index() const {
        return (t_engine_ == this) ? t_worker_index_ : k_main_thread_index;
    }
    /// @brief Number of worker threads owned by this engine.
    uint32_t worker_count() const { return num_threads_; }
    /// @brief Whether the calling thread is one of this engine's own workers.
    bool is_worker_thread() const { return t_engine_ == this; }

    // --- Submission ---

    /**
     * @brief Submits a single job.
     * @tparam F Callable, `void()` or `void(const JobContext&)`.
     * @param task_func The function to execute.
     * @param type The type category of the job (dedication / diagnostics).
     * @param handle The handle this job contributes to (must already be allocated).
     * @param dependencies Pointer to a dependency handle array (may be nullptr).
     * @param dep_count Number of dependencies. No upper bound.
     * @param priority Scheduling priority within a worker's own deque.
     */
    template<typename F>
    void submit(F&& task_func, JobType type, const JobHandle& handle,
                const JobHandle* dependencies = nullptr, uint32_t dep_count = 0,
                Priority priority = Priority::Normal) {
        handle.add_jobs_(1);
#ifdef COOPA_JOB_DIAGNOSTICS
        if (type < k_max_job_types) job_type_counts_[type].fetch_add(1, std::memory_order_relaxed);
#endif
        Job new_job(std::forward<F>(task_func), type, handle, priority);
        if (dep_count > 0) {
            dep_graph_.submit_with_dependencies(std::move(new_job), dependencies, dep_count);
            return;
        }
        dispatch_job_(std::move(new_job));
    }

    /// @brief Convenience overload taking dependencies as a span.
    template<typename F>
    void submit(F&& task_func, JobType type, const JobHandle& handle,
                std::span<const JobHandle> dependencies, Priority priority = Priority::Normal) {
        submit(std::forward<F>(task_func), type, handle,
               dependencies.data(), static_cast<uint32_t>(dependencies.size()), priority);
    }

    /**
     * @brief Submits a batch of jobs that all contribute to a single new handle.
     * @return A new JobHandle representing this batch. Must eventually be closed.
     */
    template<typename F>
    JobHandle submit_jobs(const std::vector<F>& task_funcs, JobType type,
                          const JobHandle* dependencies = nullptr, uint32_t dep_count = 0,
                          Priority priority = Priority::Normal) {
        JobHandle new_handle = create_handle();
        size_t n = task_funcs.size();
        if (n == 0 || !new_handle.is_valid()) return new_handle;

        new_handle.add_jobs_(static_cast<int32_t>(n));
#ifdef COOPA_JOB_DIAGNOSTICS
        if (type < k_max_job_types) job_type_counts_[type].fetch_add(static_cast<int>(n), std::memory_order_relaxed);
#endif
        for (const auto& task_func : task_funcs) {
            Job new_job(task_func, type, new_handle, priority);
            if (dep_count > 0) {
                dep_graph_.submit_with_dependencies(std::move(new_job), dependencies, dep_count);
            } else {
                dispatch_job_(std::move(new_job));
            }
        }
        return new_handle;
    }

    /**
     * @brief Splits [0, count) into contiguous chunks and submits one job per
     *        chunk, all contributing to the returned handle. Defined in
     *        parallel_for.h (included after this header) -- declared here so
     *        it can be called as an ordinary JobEngine method.
     * @tparam F Callable, `void(size_t begin, size_t end)` or
     *         `void(size_t begin, size_t end, const JobContext&)`.
     * @param grain Elements per chunk. 0 auto-picks count/(worker_count()*4),
     *        floored at 1.
     * @return A handle covering every chunk. Must eventually be closed.
     */
    template<typename F>
    JobHandle parallel_for(size_t count, size_t grain, F&& body,
                            JobType type = 0, Priority priority = Priority::Normal);

    /// @brief parallel_for() followed by wait_for() and close() on the resulting handle.
    template<typename F>
    /// @param help_down_to Lowest priority the waiting thread will run while it waits -- see
    ///        wait_for(handle, help_down_to). The chunks themselves run at `priority`, which must
    ///        be at or above it.
    void parallel_for_blocking(size_t count, size_t grain, F&& body,
                               JobType type = 0, Priority priority = Priority::Normal,
                               Priority help_down_to = Priority::Low);

    // --- Waiting ---

    /**
     * @brief Stalls the calling thread until the specified handle is complete.
     *
     * If called from one of this engine's own worker threads, participates as
     * that real worker (its own deque, its own dedicated queues, and stealing)
     * rather than as a steal-only guest -- this is what makes it safe for a
     * running job to wait on another job without deadlocking. Any other
     * calling thread participates as a guest (global queues + stealing only).
     *
     * @param help_down_to Lowest priority of the jobs the waiting thread will run while it
     *        waits. The default (Low) helps with anything. A frame-critical wait passes Normal,
     *        so it never picks up a long Low-priority background job (asset decode, a navmesh
     *        tile, a flow-field slice) and stalls the frame on work nobody needed this frame.
     *        Only restrict it when the awaited jobs themselves run at that priority or above --
     *        otherwise the wait relies on other threads to run them.
     */
    void wait_for(const JobHandle& handle, Priority help_down_to = Priority::Low) {
        uint32_t me = worker_index();
        while (!handle.is_complete()) {
            if (!try_execute_a_job(me, help_down_to)) std::this_thread::yield();
        }
    }

    /**
     * @brief Timed variant of wait_for().
     * @return True if the handle completed before the timeout elapsed.
     */
    bool wait_for(const JobHandle& handle, std::chrono::nanoseconds timeout) {
        uint32_t me = worker_index();
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!handle.is_complete()) {
            if (!try_execute_a_job(me)) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::yield();
            }
        }
        return true;
    }

    /// @brief Blocks until every handle in the span is complete.
    void wait_for_all(std::span<const JobHandle> handles) {
        uint32_t me = worker_index();
        for (;;) {
            bool all_done = true;
            for (const auto& h : handles) {
                if (!h.is_complete()) { all_done = false; break; }
            }
            if (all_done) return;
            if (!try_execute_a_job(me)) std::this_thread::yield();
        }
    }

    /// @brief Blocks until at least one handle in the span is complete; returns its index.
    size_t wait_for_any(std::span<const JobHandle> handles) {
        uint32_t me = worker_index();
        for (;;) {
            for (size_t i = 0; i < handles.size(); ++i) {
                if (handles[i].is_complete()) return i;
            }
            if (!try_execute_a_job(me)) std::this_thread::yield();
        }
    }

#ifdef COOPA_JOB_DIAGNOSTICS
    /// @brief Gets a snapshot of current job counts.
    Snapshot get_diagnostics_snapshot() const {
        Snapshot snapshot;
        for (uint32_t i = 0; i < k_max_job_types; ++i) {
            int c = job_type_counts_[i].load(std::memory_order_relaxed);
            snapshot.type_counts[i] = c;
            snapshot.total_pending += c;
        }
        return snapshot;
    }

    /// @brief Gathers diagnostics for all worker thread queues.
    std::vector<ThreadDiagnostics> get_thread_diagnostics() const {
        std::vector<ThreadDiagnostics> diagnostics(num_threads_);
        for (unsigned int i = 0; i < num_threads_; ++i) {
            diagnostics[i].thread_id = i;
            size_t depth = 0;
            for (auto& dq : per_thread_deques_[i]) depth += dq->size_approx();
            diagnostics[i].queue_depth = depth;
            diagnostics[i].dedication = "General";
        }
        std::lock_guard<std::mutex> lock(dedication_mutex_);
        for (uint32_t type = 0; type < k_max_job_types; ++type) {
            uint32_t thread_idx = thread_dedications_[type].load(std::memory_order_relaxed);
            if (thread_idx != UINT32_MAX && thread_idx < num_threads_) {
                std::string& d = diagnostics[thread_idx].dedication;
                d = (d == "General") ? std::to_string(type) : (d + "," + std::to_string(type));
            }
        }
        return diagnostics;
    }
#endif // COOPA_JOB_DIAGNOSTICS

    // --- Shutdown ---

    /// @brief Signals all worker threads to stop, joins them, and reports any unclosed handles.
    void shutdown() {
        if (shutting_down_.exchange(true)) return;

        if (!worker_threads_.empty() && worker_threads_[0]->joinable()) {
            logger_->info("JobEngine: Shutting down workers...");
            for (const auto& worker : worker_threads_) worker->signal_stop();
            wake_for_new_jobs_(num_threads_);
            for (auto& worker : worker_threads_) {
                if (worker->joinable()) worker->join();
            }
            worker_threads_.clear();
            logger_->info("JobEngine: All workers shut down.");
        }

        uint32_t leaked = counter_pool_.debug_outstanding_count();
        if (leaked > 0) {
            logger_->warn("JobEngine: " + std::to_string(leaked) +
                          " JobHandle(s) were never closed before shutdown (leaked slots). "
                          "Every handle from create_handle()/submit_jobs() must eventually "
                          "have close() called on it, or be wrapped in a ScopedJobHandle.");
        }
    }

private:
    /// @brief Routes a dependency-free job to its destination: a dedicated
    ///        queue if its type is dedicated, else the calling worker's own
    ///        deque (falling back to the global queue if full), else the
    ///        global queue for any non-worker caller.
    void dispatch_job_(Job&& job) {
        JobType type = job.type;
        if (any_dedications_.load(std::memory_order_acquire) && type < k_max_job_types) {
            uint32_t dedicated_worker = thread_dedications_[type].load(std::memory_order_acquire);
            if (dedicated_worker != UINT32_MAX) {
                dedicated_queues_[type].push(std::move(job));
                runnable_jobs_count_.fetch_add(1, std::memory_order_relaxed);
                wake_for_new_jobs_(1);
                return;
            }
        }

        size_t pidx = static_cast<size_t>(job.priority);
        if (is_worker_thread()) {
            uint32_t self = t_worker_index_;
            // WorkStealingDeque::push() only moves from its argument on the
            // success path (it checks capacity before touching the item), so
            // `job` is still valid here if this push failed.
            if (per_thread_deques_[self][pidx]->push(std::move(job))) {
                runnable_jobs_count_.fetch_add(1, std::memory_order_relaxed);
                wake_for_new_jobs_(1);
                return;
            }
        }

        global_queues_[pidx].push(std::move(job));
        runnable_jobs_count_.fetch_add(1, std::memory_order_relaxed);
        wake_for_new_jobs_(1);
    }

    /// @brief The main loop for each worker thread.
    void worker_thread_loop(unsigned int thread_id, std::atomic<bool>& stop_flag) {
        t_engine_ = this;
        t_worker_index_ = thread_id;

        while (!stop_flag.load(std::memory_order_acquire)) {
            bool found_work = false;
            for (uint32_t spin = 0; spin < k_worker_spin_count; ++spin) {
                if (stop_flag.load(std::memory_order_relaxed)) { t_engine_ = nullptr; return; }
                if (try_execute_a_job(thread_id)) { found_work = true; break; }
            }
            if (found_work) continue;

            std::unique_lock<std::mutex> lock(worker_mutex_);
            worker_cv_.wait(lock, [&]() {
                return stop_flag.load(std::memory_order_acquire) ||
                       runnable_jobs_count_.load(std::memory_order_acquire) > 0;
            });
        }
        t_engine_ = nullptr;
        logger_->info("Worker thread " + std::to_string(thread_id) + " exiting.");
    }

    /**
     * @brief Attempts to find and execute one job from any source.
     *
     * Priority order: this worker's dedicated queue(s) (if any) -> this
     * worker's own priority deques (High -> Normal -> Low) -> the global
     * priority queues (any thread may pop these directly) -> a randomized
     * steal from another worker's deques.
     *
     * @param worker_id This engine's worker index for the calling thread, or
     *        k_main_thread_index for a guest (no own deque -- steps 1-2 are
     *        skipped, leaving only the global queues and stealing).
     * @param lowest Lowest priority to take (see wait_for()). Anything above Low also skips
     *        the dedicated queues, whose jobs carry no priority of their own here.
     */
    bool try_execute_a_job(uint32_t worker_id, Priority lowest = Priority::Low) {
        Job job;
        const size_t levels = static_cast<size_t>(lowest) + 1;

        if (worker_id < num_threads_) {
            if (lowest == Priority::Low && any_dedications_.load(std::memory_order_acquire)) {
                for (JobType t = 0; t < k_max_job_types; ++t) {
                    if (thread_dedications_[t].load(std::memory_order_relaxed) != worker_id) continue;
                    if (dedicated_queues_[t].try_pop(job)) {
                        runnable_jobs_count_.fetch_sub(1, std::memory_order_relaxed);
                        execute_job_(job);
                        return true;
                    }
                }
            }
            for (size_t p = 0; p < levels; ++p) {
                auto& dq = per_thread_deques_[worker_id][p];
                if (dq->pop(job)) {
                    runnable_jobs_count_.fetch_sub(1, std::memory_order_relaxed);
                    execute_job_(job);
                    return true;
                }
            }
        }

        for (size_t p = 0; p < levels; ++p) {
            if (global_queues_[p].try_pop(job)) {
                runnable_jobs_count_.fetch_sub(1, std::memory_order_relaxed);
                execute_job_(job);
                return true;
            }
        }

        if (num_threads_ > 0) {
            uint32_t start = next_steal_rng_() % num_threads_;
            for (uint32_t off = 0; off < num_threads_; ++off) {
                uint32_t victim = (start + off) % num_threads_;
                if (victim == worker_id) continue;
                for (size_t p = 0; p < levels; ++p) {
                    auto& dq = per_thread_deques_[victim][p];
                    if (dq->steal(job)) {
                        runnable_jobs_count_.fetch_sub(1, std::memory_order_relaxed);
                        execute_job_(job);
                        return true;
                    }
                }
            }
        }

        return false;
    }

    /// @brief Executes a job (unless cancelled) and resolves its completion bookkeeping.
    void execute_job_(Job& job) {
        if (!job.handle.is_cancelled()) {
            JobContext ctx{worker_index(), this, job.handle};
            job.task(ctx);
        }
#ifdef COOPA_JOB_DIAGNOSTICS
        if (job.type < k_max_job_types) job_type_counts_[job.type].fetch_sub(1, std::memory_order_relaxed);
#endif
        if (job.handle.complete_job_and_check_zero_()) {
            // We are the completion that drove this handle's job counter to
            // zero -- wake any dependents parked on it, then see if the
            // creator has already closed it (in which case we reclaim now).
            dep_graph_.harvest_and_fire(job.handle);
            job.handle.try_reclaim_after_completion_();
        }
    }

    /// @brief Wakes worker(s) after new work becomes available.
    void wake_for_new_jobs_(uint32_t n) {
        if (n == 0) return;
        std::lock_guard<std::mutex> lock(worker_mutex_);
        if (n == 1) worker_cv_.notify_one();
        else worker_cv_.notify_all();
    }

    /// @brief Per-thread xorshift32 stream for randomized steal victim selection.
    static uint32_t next_steal_rng_() {
        static thread_local uint32_t state = [] {
            uint32_t seed = static_cast<uint32_t>(
                std::hash<std::thread::id>{}(std::this_thread::get_id()));
            return seed != 0 ? seed : 0xA5A5A5A5u;
        }();
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    coopa::debug::Logger* logger_ = nullptr;
    unsigned int num_threads_;
    CounterPool counter_pool_;      /**< Generation-tagged handle pool, see handle.h. */
    DependencyGraph dep_graph_;      /**< Event-driven dependency resolution, see dependency_graph.h. */
    std::vector<std::unique_ptr<coopa::job::Thread>> worker_threads_;

    /// @brief Per-worker, per-priority lock-free deques (owner push/pop only; any thread may steal).
    std::vector<std::array<std::unique_ptr<WorkStealingDeque<Job>>, k_priority_count>> per_thread_deques_;

    /// @brief Global priority queues: destination for non-owner submits, deque overflow, and
    ///        jobs released ready by the dependency graph. Safe for any thread to push/pop.
    std::array<ParallelQueue<Job>, k_priority_count> global_queues_;

    /// @brief One hard-dedicated queue per possible JobType; only the dedicated worker ever pops it.
    std::array<ParallelQueue<Job>, k_max_job_types> dedicated_queues_;
    std::array<std::atomic<uint32_t>, k_max_job_types> thread_dedications_; /**< JobType -> worker index, or UINT32_MAX. */
    std::atomic<bool> any_dedications_{false}; /**< Fast-path skip for the common case of no dedications. */
    mutable std::mutex dedication_mutex_;      /**< Guards dedicate_thread_to_job_type() writers. */

    /// @brief Count of jobs currently sitting in a deque/queue, ready to run (excludes
    ///        jobs still parked on unmet dependencies). Drives the worker sleep predicate.
    alignas(k_cache_line_size) std::atomic<int> runnable_jobs_count_{0};

    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::atomic<bool> shutting_down_{false};

    static inline thread_local JobEngine* t_engine_ = nullptr;      /**< Which engine owns this thread, if any. */
    static inline thread_local uint32_t t_worker_index_ = k_main_thread_index; /**< This thread's worker index within t_engine_. */

#ifdef COOPA_JOB_DIAGNOSTICS
    std::array<std::atomic<int>, k_max_job_types> job_type_counts_;
#endif
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_ENGINE_H
