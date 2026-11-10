/**
 * @file log_format.h
 * @brief Shared console-formatting helpers for the debug loggers.
 *
 * Internal to coopa::debug. Logger, DebugManager and DebugBucket all render the
 * same timestamp and the same `[LEVEL]::[scope]::[tag] message` shape; this is
 * where that formatting lives so the three cannot drift apart.
 */

#ifndef COOPA_DEBUG_DETAIL_LOG_FORMAT_H
#define COOPA_DEBUG_DETAIL_LOG_FORMAT_H

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include <coopa/debug/message.h>
#include <coopa/job/collections/queue.h>

namespace coopa {
namespace debug {
namespace detail {

/**
 * @brief Renders a time point as `HH:MM:SS.mmm` in local time.
 *
 * Deliberately time-of-day only: these logs are read alongside a running frame
 * loop, where the date is noise.
 *
 * @param when The time point to format.
 * @return The formatted timestamp.
 */
inline std::string format_timestamp(std::chrono::system_clock::time_point when) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) % 1000;
    std::time_t time = std::chrono::system_clock::to_time_t(when);
    std::tm tm_snapshot;

#ifdef _WIN32
    localtime_s(&tm_snapshot, &time);   // MSVC's thread-safe localtime.
#else
    localtime_r(&time, &tm_snapshot);   // POSIX's thread-safe localtime.
#endif

    std::stringstream ss;
    ss << std::put_time(&tm_snapshot, "%H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

/**
 * @brief Drains a log queue, orders it by timestamp, and writes it out.
 *
 * Messages arrive from many worker threads in whatever order those threads got
 * to the queue, so they are only chronological once sorted here.
 *
 * @param queue The queue to drain. Left empty.
 * @param out Destination stream.
 * @param scope Scope tag inserted after the level, e.g. `"[JOBS]::"`. May be
 *              empty for collectors that have no sub-scope of their own.
 */
inline void drain_sorted(coopa::job::ParallelQueue<LogMessage>& queue,
                         std::ostream& out,
                         const std::string& scope) {
    std::vector<LogMessage> messages;
    queue.pop_all(messages);
    std::sort(messages.begin(), messages.end());

    for (const LogMessage& log_msg : messages) {
        const char* origin = log_msg.is_main_thread ? "(main)" : "(parallel)";
        out << format_timestamp(log_msg.timestamp)
            << " [INFO]::" << scope << origin
            << "::[\e[3m" << log_msg.type << "\e[0m]"
            << " " << log_msg.message << std::endl;
    }
}

} // namespace detail
} // namespace debug
} // namespace coopa

#endif // COOPA_DEBUG_DETAIL_LOG_FORMAT_H
