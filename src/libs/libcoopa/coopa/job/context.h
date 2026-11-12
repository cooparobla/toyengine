/**
 * @file context.h
 * @brief Job categorization, priority, and the per-invocation JobContext.
 *
 * JobType and Priority are the namespaced vocabulary the whole job system
 * categorizes and orders work by; they live here so job.h and engine.h share
 * one definition.
 * JobContext is handed to any job body that opts in to receiving it (see
 * TaskWrapper's dual-signature dispatch in job.h), giving a running job access
 * to which worker it is executing on and whether it has been cancelled.
 */

#ifndef COOPA_JOB_CONTEXT_H
#define COOPA_JOB_CONTEXT_H

#include <cstdint>

#include <coopa/job/handle.h>

namespace coopa {
namespace job {

/// @brief Categorization tag for jobs (used for dedication and metrics).
using JobType = uint32_t;

/**
 * @brief Scheduling priority. Lower numeric value runs first when a worker has
 *        a choice (its own deque is priority-ordered High -> Normal -> Low).
 */
enum class Priority : uint8_t {
    High   = 0,
    Normal = 1,
    Low    = 2,
    Count  = 3,
};

/// @brief worker_index() sentinel meaning "not a worker thread of this engine".
inline constexpr uint32_t k_main_thread_index = UINT32_MAX;

class JobEngine; // Forward declaration only -- see engine.h.

/**
 * @struct JobContext
 * @brief Passed to any job body written as `void(const JobContext&)` instead of
 *        the plain `void()` form.
 */
struct JobContext {
    uint32_t worker_index = k_main_thread_index; /**< 0..worker_count()-1, or k_main_thread_index. */
    JobEngine* engine = nullptr;                 /**< The engine executing this job. */
    JobHandle group;                             /**< The handle this job contributes to. */

    /// @brief Whether JobEngine::cancel() has been called on this job's group handle.
    bool is_cancelled() const { return group.is_cancelled(); }
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_CONTEXT_H
