/**
 * @file logger.h
 * @brief Defines the thread-safe Logger class for writing formatted logs to stderr.
 */

#ifndef COOPA_DEBUG_LOGGER_H
#define COOPA_DEBUG_LOGGER_H

#include <chrono>
#include <iostream>
#include <string>
#include <mutex>     // For std::mutex and std::lock_guard

#include <coopa/debug/detail/log_format.h>
#include <coopa/debug/printer.h>

namespace coopa
{
namespace debug
{

/**
 * @class Logger
 * @brief Thread-safe synchronous logger that formats outputs with timestamps and level tags.
 */
class Logger : public Printer {
public:
    /**
     * @brief Constructs a Logger instance.
     * @param name The identification tag for this logger instance (printed in brackets).
     */
    Logger(const std::string& name) : Printer(), name_(name) {}

    /**
     * @brief Virtual destructor.
     */
    virtual ~Logger() = default;

    /**
     * @brief Logs an informational message.
     * @param message Message to log.
     * @param type Category tag (unused by direct Logger).
     * @param is_main_thread Main thread flag (unused by direct Logger).
     * @param indent Indentation level.
     */
    void info(const std::string& message, std::string type = "", bool is_main_thread = false, unsigned int indent = 0) override { 
        log("INFO", message, indent); 
    } 

    /**
     * @brief Logs a warning message.
     * @param message Message to log.
     */
    void warn(const std::string& message) { log("WARN", message); } 

    /**
     * @brief Logs an error message.
     * @param message Message to log.
     */
    void error(const std::string& message) { log("ERROR", message); } 

private:
    /**
     * @brief Formats and writes the log message to stderr.
     * @param level String tag representing the log severity (e.g. "INFO").
     * @param message The content of the log message.
     * @param indent The indentation level for tree formatting.
     */
    void log(std::string level, const std::string& message, unsigned int indent = 0) {
        // RAII lock so the mutex is always released, and so the whole line is
        // written as one operation -- otherwise concurrent loggers interleave
        // mid-line on the console.
        std::lock_guard<std::mutex> lock(mtx_);

        const std::string stamp = detail::format_timestamp(std::chrono::system_clock::now());
        if (indent > 0) {
            std::cerr << stamp << " [" << level << "]::[\e[3m" << name_ << "\e[0m]"
                      << std::string(indent, '\t') << "- " << message << std::endl;
        } else {
            std::cerr << stamp << " [" << level << "]::[\e[3m" << name_ << "\e[0m]"
                      << " " << message << std::endl;
        }
    }

    std::string name_; /**< Name of this logger instance. */

    // `inline static` so the one mutex is shared across every translation
    // unit that includes this header, rather than one per TU.
    inline static std::mutex mtx_; /**< Mutex to ensure console printing is thread-safe. */
};

}
}

#endif  // COOPA_DEBUG_LOGGER_H