/**
 * @file scheduler.h
 * @brief Automatic hazard-aware job scheduler and dependency resolver.
 *
 * Manages batches of jobs with automatic dependency injection based on
 * component resource access patterns (RAW, WAR, WAW hazards). Includes
 * per-frame lifecycle management to prevent memory leaks from accumulated
 * tracking state.
 */

#ifndef COOPA_JOB_SCHEDULER_H
#define COOPA_JOB_SCHEDULER_H

#include <algorithm>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <coopa/job/context.h>
#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/job/job.h>
#include <coopa/job/platform.h>

namespace coopa {
namespace job {

/**
 * @struct JobNodeDefinition
 * @brief Defines a job's task callback and its component read/write access patterns.
 *
 * `task` is a TaskWrapper (not std::function<void()>) so a job registered
 * through JobScheduler pays the same small-buffer-optimized, allocation-free
 * cost as one submitted directly to JobEngine.
 */
struct JobNodeDefinition {
    TaskWrapper task;            /**< The job's work. */
    JobType type = 0;            /**< Job type category. */
    std::vector<std::type_index> read_component_types;  /**< Component type IDs this job reads. */
    std::vector<std::type_index> write_component_types; /**< Component type IDs this job writes. */
    bool is_main_thread_job = false; /**< True if this job must execute on the main/render thread. */

    JobNodeDefinition() = default;

    /**
     * @brief Constructs an explicit JobNodeDefinition.
     * @tparam F Callable, `void()` or `void(const JobContext&)`.
     */
    template<typename F>
    JobNodeDefinition(
        F&& fn,
        JobType t,
        std::vector<std::type_index> reads = {},
        std::vector<std::type_index> writes = {},
        bool is_main_thread = false
    ) :
        task(std::forward<F>(fn)),
        type(t),
        read_component_types(std::move(reads)),
        write_component_types(std::move(writes)),
        is_main_thread_job(is_main_thread)
    {}
};

/**
 * @struct JobNode
 * @brief A main-thread job parked until execute_main_thread_jobs() runs it.
 *
 * Unlike a worker-thread job (submitted straight to JobEngine, which resolves
 * its dependencies via the event-driven dependency graph), a main-thread
 * job's dependencies are waited on explicitly, right before running its
 * task, by execute_main_thread_jobs() -- see that method's doc.
 */
struct JobNode {
    TaskWrapper task;                    /**< The job's work. */
    JobHandle handle;                    /**< This job's own completion handle. */
    std::vector<JobHandle> dependencies; /**< Waited on (via wait_for_all) before task runs. */
};

/**
 * @class JobScheduler
 * @brief Manages a batch of jobs and their dependencies based on component resource access.
 *
 * The scheduler automatically forms dependencies by tracking component resource contention:
 * - Read-After-Write (RAW): a reader must wait for the prior writer.
 * - Write-After-Read (WAR): a writer must wait for all prior readers.
 * - Write-After-Write (WAW): a writer must wait for the prior writer.
 *
 * Dependency lists are unbounded and never truncated, since JobEngine's own
 * dependency graph (see dependency_graph.h) has no inline-count limit either.
 *
 * begin_frame()/end_frame() clear this scheduler's own hazard-tracking maps
 * every frame. That state is genuinely per-frame bookkeeping, and is separate
 * from JobEngine's frame boundary, which is diagnostics-only.
 */
class JobScheduler {
public:
    /**
     * @brief Constructs a JobScheduler associated with a JobEngine.
     * @param engine The JobEngine reference where jobs will be submitted.
     */
    explicit JobScheduler(JobEngine& engine) : engine_(engine) {}

    // --- Frame Lifecycle ---

    /**
     * @brief Signals the start of a new frame.
     *
     * Clears all persistent hazard-tracking state (last-writer and active-reader
     * maps) to prevent unbounded memory growth. Also delegates to the engine's
     * begin_frame() (diagnostics only).
     *
     * Must be called at the start of each frame/update cycle before any add_job() calls.
     */
    void begin_frame() {
        std::lock_guard<std::mutex> lock(m_mutex_);
        persistent_last_writer_handles_.clear();
        persistent_active_reader_handles_.clear();
        engine_.begin_frame();
    }

    /**
     * @brief Signals the end of a frame.
     *
     * Drops any main-thread job that execute_main_thread_jobs() was never
     * called to run this frame, then delegates to the engine's end_frame().
     * Every main-thread job queued via add_job()/submit_all() is expected to
     * be run by execute_main_thread_jobs() before end_frame() -- this is a
     * defensive fallback, not the intended path.
     *
     * Dropping a job still has to resolve its handle exactly as running it
     * would: submit_all() charged the handle one outstanding job, and
     * CounterPool::close() can only reclaim a slot whose counter has already
     * reached zero. Closing without that decrement would strand the slot
     * (never returned to the free list) and leave anything that took a
     * dependency on this job waiting forever.
     */
    void end_frame() {
        std::lock_guard<std::mutex> lock(m_mutex_);
        for (auto& node : main_thread_jobs_queue_) {
            engine_.complete_external_job(node.handle); // Drives the counter to 0 and fires dependents.
            node.handle.close();                        // Now actually reclaims the slot.
        }
        main_thread_jobs_queue_.clear();
        pending_job_definitions_.clear();
        engine_.end_frame();
    }

    // --- Job Registration ---

    /**
     * @brief Enqueues a job definition.
     *
     * Dependencies are automatically calculated upon submission based on
     * read/write component access patterns.
     *
     * @tparam F Callable, `void()` or `void(const JobContext&)`.
     * @param task The work function.
     * @param type The category tag of the job.
     * @param read_types Component Type IDs this job reads.
     * @param write_types Component Type IDs this job writes.
     * @param main_thread True if execution must occur on the main thread.
     */
    template<typename F>
    void add_job(
        F&& task,
        JobType type,
        const std::vector<std::type_index>& read_types = {},
        const std::vector<std::type_index>& write_types = {},
        bool main_thread = false
    ) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        pending_job_definitions_.emplace_back(
            std::forward<F>(task), type, read_types, write_types, main_thread);
    }

    // --- Submission ---

    /**
     * @brief Resolves dependency hazards and submits all enqueued jobs to the JobEngine.
     *
     * Builds a dependency graph by analyzing read/write access patterns across
     * all pending job definitions, then submits each job with its computed
     * dependencies (worker-thread jobs go straight to JobEngine::submit();
     * main-thread jobs are parked for execute_main_thread_jobs()).
     *
     * @return A vector of JobHandles representing the submitted jobs. Every
     *         one must eventually be closed (wait_for_all() below does not
     *         do this for you).
     */
    std::vector<JobHandle> submit_all() {
        std::lock_guard<std::mutex> lock(m_mutex_);

        std::vector<JobHandle> new_job_handles;
        new_job_handles.reserve(pending_job_definitions_.size());

        std::unordered_map<std::type_index, JobHandle> last_writer_handles = std::move(persistent_last_writer_handles_);
        std::unordered_map<std::type_index, std::vector<JobHandle>> active_reader_handles = std::move(persistent_active_reader_handles_);
        std::unordered_set<std::type_index> write_type_set;

        for (JobNodeDefinition& def : pending_job_definitions_) {
            std::vector<JobHandle> dependencies;

            for (const std::type_index& read_type : def.read_component_types) {
                auto it = last_writer_handles.find(read_type);
                if (it != last_writer_handles.end()) dependencies.push_back(it->second);
            }

            for (const std::type_index& write_type : def.write_component_types) {
                auto it_readers = active_reader_handles.find(write_type);
                if (it_readers != active_reader_handles.end()) {
                    dependencies.insert(dependencies.end(), it_readers->second.begin(), it_readers->second.end());
                    active_reader_handles.erase(it_readers);
                }
                auto it_writer = last_writer_handles.find(write_type);
                if (it_writer != last_writer_handles.end()) dependencies.push_back(it_writer->second);
            }

            std::sort(dependencies.begin(), dependencies.end(),
                      [](const JobHandle& a, const JobHandle& b) { return a.index() < b.index(); });
            dependencies.erase(
                std::unique(dependencies.begin(), dependencies.end(),
                            [](const JobHandle& a, const JobHandle& b) { return a.index() == b.index(); }),
                dependencies.end());

            JobHandle self_handle = engine_.create_handle();

            if (def.is_main_thread_job) {
                self_handle.add_jobs_(1);
                JobNode node;
                node.task = std::move(def.task);
                node.handle = self_handle;
                node.dependencies = std::move(dependencies);
                main_thread_jobs_queue_.push_back(std::move(node));
            } else {
                engine_.submit(std::move(def.task), def.type, self_handle,
                                dependencies.data(), static_cast<uint32_t>(dependencies.size()));
            }

            new_job_handles.push_back(self_handle);

            for (const std::type_index& write_type : def.write_component_types) {
                last_writer_handles[write_type] = self_handle;
            }

            write_type_set.clear();
            for (const std::type_index& wt : def.write_component_types) write_type_set.insert(wt);

            for (const std::type_index& read_type : def.read_component_types) {
                if (write_type_set.find(read_type) == write_type_set.end()) {
                    active_reader_handles[read_type].push_back(self_handle);
                }
            }
        }

        persistent_last_writer_handles_ = std::move(last_writer_handles);
        persistent_active_reader_handles_ = std::move(active_reader_handles);
        pending_job_definitions_.clear();

        return new_job_handles;
    }

    // --- Main Thread Execution ---

    /**
     * @brief Executes every enqueued main-thread job, in submission order,
     *        on the calling (main/render) thread.
     *
     * For each job, first blocks (via JobEngine::wait_for_all(), which lets
     * this thread help execute other work while it waits) until every one of
     * that job's dependencies is complete, then runs its task, then resolves
     * its own completion bookkeeping via JobEngine::complete_external_job()
     * so anything depending on THIS job is woken correctly. Must be called
     * once per frame, after submit_all().
     */
    void execute_main_thread_jobs() {
        std::vector<JobNode> jobs;
        {
            std::lock_guard<std::mutex> lock(m_mutex_);
            jobs = std::move(main_thread_jobs_queue_);
            main_thread_jobs_queue_.clear();
        }

        for (JobNode& node : jobs) {
            if (!node.dependencies.empty()) {
                engine_.wait_for_all(node.dependencies);
            }
            JobContext ctx{engine_.worker_index(), &engine_, node.handle};
            node.task(ctx);
            engine_.complete_external_job(node.handle);
        }
    }

    // --- Waiting ---

    /// @brief Blocks the current thread until every handle in the batch is complete.
    void wait_for_all(const std::vector<JobHandle>& batch_handles) {
        engine_.wait_for_all(batch_handles);
    }

private:
    JobEngine& engine_; /**< Reference to the core job engine. */
    std::vector<JobNodeDefinition> pending_job_definitions_; /**< Pending job definitions. */
    std::vector<JobNode> main_thread_jobs_queue_; /**< Queue of main-thread-only jobs. */
    mutable std::mutex m_mutex_; /**< Mutex for thread-safety. */

    /// @brief Tracks last writer handle per component type (cleared per-frame via begin_frame).
    std::unordered_map<std::type_index, JobHandle> persistent_last_writer_handles_;

    /// @brief Tracks active reader handles per component type (cleared per-frame via begin_frame).
    std::unordered_map<std::type_index, std::vector<JobHandle>> persistent_active_reader_handles_;
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_SCHEDULER_H
