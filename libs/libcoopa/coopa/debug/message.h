/**
 * @file message.h
 * @brief Defines the LogMessage structure representing a log entry.
 */

#ifndef COOPA_DEBUG_MESSAGE_H
#define COOPA_DEBUG_MESSAGE_H

#include <chrono>
#include <string>

namespace coopa {
namespace debug {

/**
 * @struct LogMessage
 * @brief Representation of a logged message.
 */
struct LogMessage {
    std::chrono::system_clock::time_point timestamp; /**< The timestamp when the message was logged. */
    std::string type; /**< Category tag of the log. */
    std::string message; /**< Content of the log message. */

    bool is_main_thread = false; /**< True if logged from the main thread. */

    /**
     * @brief Default constructor (required for queue containers).
     */
    LogMessage() = default; 

    /**
     * @brief Constructs a LogMessage with full details.
     * @param ts Timestamp.
     * @param type_str Log category.
     * @param msg Log content.
     * @param is_main_thread Thread flag.
     */
    LogMessage(std::chrono::system_clock::time_point ts, std::string type_str, std::string msg, bool is_main_thread = false)
        : timestamp(std::move(ts)), type(std::move(type_str)), message(std::move(msg)), is_main_thread(is_main_thread) {}

    /**
     * @brief Compares two LogMessages by their timestamp.
     * @param other The other log message to compare with.
     * @return True if this message has an earlier timestamp than the other.
     */
    bool operator<(const LogMessage& other) const {
        return timestamp < other.timestamp;
    }
};

}
}

#endif // COOPA_DEBUG_MESSAGE_H