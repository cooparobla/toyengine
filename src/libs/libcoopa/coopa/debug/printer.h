/**
 * @file printer.h
 * @brief Defines the Printer abstract base class for log output.
 */

#ifndef COOPA_DEBUG_PRINTER_H
#define COOPA_DEBUG_PRINTER_H

#include <string>

namespace coopa {
namespace debug {

/**
 * @class Printer
 * @brief Interface for logging messages.
 */
class Printer {
public:
    /**
     * @brief Constructs a Printer.
     */
    Printer() {}

    /**
     * @brief Virtual destructor for clean inheritance teardown.
     */
    virtual ~Printer() = default; 

    /**
     * @brief Logs an informational message.
     * @param message The content of the message.
     * @param type An optional category tag for the message.
     * @param is_main_thread True if logged from the main thread, false otherwise.
     * @param indent The indentation level for formatting nested output.
     */
    virtual void info(const std::string& message, std::string type = "", bool is_main_thread = false, unsigned int indent = 0) {}

private:
};

}
}

#endif // COOPA_DEBUG_PRINTER_H