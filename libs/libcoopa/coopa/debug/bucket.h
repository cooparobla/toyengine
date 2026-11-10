/**
 * @file bucket.h
 * @brief Defines the DebugBucket class for collecting and printing logs in bucketed formats.
 */

#ifndef COOPA_DEBUG_BUCKET_H
#define COOPA_DEBUG_BUCKET_H

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
 * @class DebugBucket
 * @brief A container bucket to collect parallel logs, sorting them by timestamp before output.
 *
 * Identical to DebugManager except that it prints without a scope tag.
 *
 * Provided for consumers; libcoopa itself does not log through it.
 */
class DebugBucket {
public:
    /**
     * @brief Constructs a DebugBucket.
     */
    DebugBucket() = default;

    /**
     * @brief Pushes a message into the bucket's queue with the current timestamp.
     * @param type_str Log category tag.
     * @param message Log message.
     */
    void info(const std::string& type_str, const std::string& message) {
        log_queue_.push(LogMessage(std::chrono::system_clock::now(), type_str, message));
    }

    /**
     * @brief Sorts and prints all queued log messages to console.
     */
    void show() {
        detail::drain_sorted(log_queue_, std::cout, "");
    }

    /**
     * @brief Retrieves the associated DebugContext.
     * @return Reference to DebugContext.
     */
    DebugContext& get_context() {
        return context_;
    }

private:
    coopa::job::ParallelQueue<LogMessage> log_queue_; /**< Thread-safe queue storing logged messages. */
    DebugContext context_ = DebugContext(log_queue_); /**< Associated log context wrapper. */
};

}
}

#endif // COOPA_DEBUG_BUCKET_H
