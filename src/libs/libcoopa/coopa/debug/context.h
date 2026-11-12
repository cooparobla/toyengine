/**
 * @file context.h
 * @brief Defines the DebugContext class for submitting log messages to a parallel queue.
 */

#ifndef COOPA_DEBUG_CONTEXT_H
#define COOPA_DEBUG_CONTEXT_H

#include <coopa/job/collections/queue.h>
#include <coopa/debug/message.h>

#include <chrono>
#include <vector>
#include <string>
#include <algorithm> // For std::sort
#include <iostream>
#include <iomanip>   // For std::put_time
#include <sstream>   // For std::stringstream

#include <coopa/debug/printer.h>

namespace coopa {
namespace debug {

/**
 * @class DebugContext
 * @brief Thread-safe logging context that pushes logs into a ParallelQueue.
 */
class DebugContext : public Printer {
public:
    /**
     * @brief Constructs a DebugContext associated with a specific queue.
     * @param log_queue The ParallelQueue to push log messages into.
     */
    DebugContext(coopa::job::ParallelQueue<LogMessage>& log_queue) : Printer(), log_queue_(log_queue) {}

    /**
     * @brief Pushes a new log message into the queue with the current system time.
     * @param message Log message text.
     * @param type_str Log category/type.
     * @param is_main_thread Set to true if logged from the main thread.
     * @param indent Indentation formatting (unused by queue loggers).
     */
    void info(const std::string& message, std::string type_str = "", bool is_main_thread = false, unsigned int indent = 0) override {
        auto now = std::chrono::system_clock::now();
        log_queue_.push(LogMessage(now, type_str, message, is_main_thread));
    }

private:
    coopa::job::ParallelQueue<LogMessage>& log_queue_; /**< Reference to the destination queue. */
};

}
}

#endif // COOPA_DEBUG_CONTEXT_H