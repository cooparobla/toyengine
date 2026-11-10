/**
 * @file diagnostics.h
 * @brief Per-node errors, warnings and timings gathered while a graph is validated,
 *        compiled or evaluated.
 *
 * Every entry point in this module reports failure by *filling a Diagnostics* rather than
 * throwing. That is deliberate: a graph is authored data, so a bad graph is an ordinary
 * runtime condition that a tool has to display, not an exceptional one. A node that throws
 * during evaluation is caught at the chain boundary and turned into an entry here, so one
 * broken node yields a message instead of taking down a worker thread.
 *
 * `add()` is safe to call from several worker threads at once; everything else is
 * single-threaded and meant for the owner once evaluation has finished.
 */

#ifndef COOPA_NODEGRAPH_DIAGNOSTICS_H
#define COOPA_NODEGRAPH_DIAGNOSTICS_H

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coopa {
namespace nodegraph {

/// @brief How bad one diagnostic entry is.
enum class Severity : uint8_t {
    Warning, ///< The graph still produced a result; something was ignored or clamped.
    Error,   ///< A node or a link could not be processed. Downstream inputs fall back to defaults.
};

/**
 * @struct Diagnostic
 * @brief One message, attached to the authored node it came from where that is known.
 */
struct Diagnostic {
    Severity    severity = Severity::Error;
    uint32_t    node = 0;          ///< The authored node id, or 0 when the message is graph-wide.
    std::string node_label;        ///< The node's type name, for a message a human can place.
    std::string message;
};

/**
 * @class Diagnostics
 * @brief A thread-safe sink for Diagnostic entries, plus optional per-node timings.
 */
class Diagnostics {
public:
    Diagnostics() = default;

    /**
     * @brief Movable, so a result struct carrying one can be returned by value.
     *
     * The mutex is not moved -- a fresh one is default-constructed and the source's entries are
     * moved out under its own lock. Moving a Diagnostics that another thread is still writing to
     * is a programming error; by construction that cannot happen, because a move only happens
     * once an evaluation has joined every job that could have been writing.
     */
    Diagnostics(Diagnostics&& other) noexcept {
        std::lock_guard<std::mutex> lock(other.mutex_);
        entries_     = std::move(other.entries_);
        timings_     = std::move(other.timings_);
        error_count_ = other.error_count_;
        other.error_count_ = 0;
    }

    Diagnostics& operator=(Diagnostics&& other) noexcept {
        if (this == &other) return *this;
        std::scoped_lock lock(mutex_, other.mutex_);
        entries_     = std::move(other.entries_);
        timings_     = std::move(other.timings_);
        error_count_ = other.error_count_;
        other.error_count_ = 0;
        return *this;
    }

    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

    /// @brief Appends one entry. Safe to call concurrently from worker threads.
    void add(Severity severity, uint32_t node, std::string node_label, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back({severity, node, std::move(node_label), std::move(message)});
        if (severity == Severity::Error) ++error_count_;
    }

    /// @brief Appends a graph-wide error with no owning node.
    void error(std::string message) { add(Severity::Error, 0, {}, std::move(message)); }
    /// @brief Appends a graph-wide warning with no owning node.
    void warn(std::string message) { add(Severity::Warning, 0, {}, std::move(message)); }

    /**
     * @brief Records how long one node took. Only collected when diagnostics are compiled in,
     *        because the clock read is not free on a node that runs in nanoseconds.
     */
    void record_time(uint32_t node, double milliseconds) {
#ifdef COOPA_NODEGRAPH_DIAGNOSTICS
        std::lock_guard<std::mutex> lock(mutex_);
        timings_.push_back({node, milliseconds});
#else
        (void)node;
        (void)milliseconds;
#endif
    }

    /// @brief True when nothing failed. Warnings do not make a result unusable.
    bool ok() const { return error_count_ == 0; }
    size_t error_count() const { return error_count_; }

    const std::vector<Diagnostic>& entries() const { return entries_; }
    const std::vector<std::pair<uint32_t, double>>& timings() const { return timings_; }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        timings_.clear();
        error_count_ = 0;
    }

    /// @brief Every entry on one line each, for a log or a test failure message.
    std::string to_string() const {
        std::string out;
        for (const auto& e : entries_) {
            out += (e.severity == Severity::Error ? "[error] " : "[warn]  ");
            if (!e.node_label.empty()) {
                out += e.node_label;
                out += " (#" + std::to_string(e.node) + "): ";
            }
            out += e.message;
            out += '\n';
        }
        return out;
    }

private:
    mutable std::mutex                       mutex_;
    std::vector<Diagnostic>                  entries_;
    std::vector<std::pair<uint32_t, double>> timings_;
    size_t                                   error_count_ = 0;
};

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_DIAGNOSTICS_H
