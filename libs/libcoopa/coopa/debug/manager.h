/**
 * @file manager.h
 * @brief Defines the DebugManager class for logging diagnostics and managing the log queue.
 */

#ifndef COOPA_DEBUG_MANAGER_H
#define COOPA_DEBUG_MANAGER_H

#include <chrono>
#include <iostream>
#include <string>

#include <coopa/debug/context.h>
#include <coopa/debug/detail/log_format.h>
#include <coopa/debug/message.h>
#include <coopa/job/collections/queue.h>

namespace coopa {
namespace debug {

/**
 * @class DebugManager
 * @brief Manager class that collects, sorts, and prints queue-based diagnostic logs.
 *
 * Collects under a `[JOBS]` scope. See DebugBucket for the unscoped equivalent.
 *
 * Provided for consumers; libcoopa itself does not log through it.
 */
class DebugManager {
public:
    /**
     * @brief Constructs a DebugManager.
     */
    DebugManager() = default;

    /**
     * @brief Submits a log message into the queue with the current timestamp.
     * @param type_str Log category.
     * @param message Log message text.
     */
    void info(const std::string& type_str, const std::string& message) {
        log_queue_.push(LogMessage(std::chrono::system_clock::now(), type_str, message));
    }

    /**
     * @brief Flushes all collected log messages, sorts them chronologically, and prints to console.
     */
    void show() {
        detail::drain_sorted(log_queue_, std::cout, "[JOBS]::");
    }

    /**
     * @brief Returns a reference to the internal DebugContext.
     * @return Reference to DebugContext.
     */
    DebugContext& get_context() {
        return context_;
    }

private:
    coopa::job::ParallelQueue<LogMessage> log_queue_; /**< Thread-safe queue of messages. */
    DebugContext context_ = DebugContext(log_queue_); /**< Associated log context interface. */
};

}
}

#endif // COOPA_DEBUG_MANAGER_H
