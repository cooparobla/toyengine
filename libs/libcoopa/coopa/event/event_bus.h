/**
 * @file event_bus.h
 * @brief Named (object name, signal name) pub/sub, built on top of Signal.
 *
 * Signal<Args...> (signal.h) is a strongly-typed, directly-connected multicast
 * primitive — a listener must hold a pointer/reference to the exact Signal it
 * wants. EventBus adds a loosely-coupled layer on top: any component can emit
 * a named signal "as" a named object without either side knowing the other's
 * concrete type, and any component can listen for a given (object name,
 * signal name) pair without holding a pointer to the emitter at all —
 * analogous to Unity's SendMessage, but typed at the argument-bag level
 * instead of fully stringly-typed.
 *
 * Header-only, STL-only — zero dependencies beyond signal.h, consistent with
 * coopa::scene's dependency-free mandate (coopa::scene::Scene owns one).
 */

#ifndef COOPA_EVENT_EVENT_BUS_H
#define COOPA_EVENT_EVENT_BUS_H

#include <coopa/event/signal.h>

#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace coopa {
namespace event {

/**
 * @class EventValue
 * @brief A single named signal argument: bool, integer, floating-point, or string.
 *
 * Covers every payload shape currently needed in this codebase (positions
 * pack as two doubles, button/key indices as an integer, ...). Construction
 * is implicit so call sites read naturally: `args.set("x", pos.x)`.
 */
class EventValue {
public:
    EventValue() = default;
    EventValue(bool v) : value_(v) {}
    EventValue(int v) : value_(static_cast<int64_t>(v)) {}
    EventValue(int64_t v) : value_(v) {}
    EventValue(float v) : value_(static_cast<double>(v)) {}
    EventValue(double v) : value_(v) {}
    EventValue(std::string v) : value_(std::move(v)) {}
    EventValue(const char* v) : value_(std::string(v)) {}

    /** @brief Whether the stored value's category matches T (bool / integral / floating-point / string). */
    template<typename T>
    bool is() const {
        if constexpr (std::is_same_v<T, bool>) {
            return std::holds_alternative<bool>(value_);
        } else if constexpr (std::is_integral_v<T>) {
            return std::holds_alternative<int64_t>(value_);
        } else if constexpr (std::is_floating_point_v<T>) {
            return std::holds_alternative<double>(value_);
        } else if constexpr (std::is_convertible_v<T, std::string>) {
            return std::holds_alternative<std::string>(value_);
        } else {
            return false;
        }
    }

    /**
     * @brief Returns the stored value as T, or fallback if the category doesn't match.
     *
     * Never throws — a mismatched request (e.g. as<std::string> on a numeric
     * value) is a silent miss, not an exception, matching this codebase's
     * "missing key falls back to the caller-supplied default" convention
     * elsewhere (e.g. ui_yaml.h's parse_vec2).
     */
    template<typename T>
    T as(T fallback) const {
        if constexpr (std::is_same_v<T, bool>) {
            if (const auto* v = std::get_if<bool>(&value_)) return *v;
            return fallback;
        } else if constexpr (std::is_integral_v<T>) {
            if (const auto* v = std::get_if<int64_t>(&value_)) return static_cast<T>(*v);
            return fallback;
        } else if constexpr (std::is_floating_point_v<T>) {
            if (const auto* v = std::get_if<double>(&value_)) return static_cast<T>(*v);
            return fallback;
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (const auto* v = std::get_if<std::string>(&value_)) return *v;
            return fallback;
        } else {
            return fallback;
        }
    }

    /**
     * @brief Renders the stored value as text, whatever its category — bool
     *        as "true"/"false", numbers via std::to_string, strings verbatim,
     *        an empty (unset) value as "".
     *
     * Used for `{key}` placeholder substitution (see uicoopa's TextOnSignal)
     * where the reactor doesn't know ahead of time which category a given
     * signal's argument will be.
     */
    std::string to_string() const {
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) return "";
            else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::string>) return v;
            else return std::to_string(v); // int64_t, double
        }, value_);
    }

private:
    std::variant<std::monostate, bool, int64_t, double, std::string> value_;
};

/**
 * @class EventArgs
 * @brief A named signal's argument bag: order-independent key -> EventValue.
 *
 * @code
 * EventArgs args;
 * args.set("x", pos.x).set("y", pos.y).set("button", 0);
 * scene.events().emit(owner->name(), "hover_enter", args);
 * @endcode
 */
class EventArgs {
public:
    EventArgs() = default;

    /** @brief Sets key to v, returning *this for chained set() calls. */
    EventArgs& set(std::string key, EventValue v) {
        values_[std::move(key)] = std::move(v);
        return *this;
    }

    /** @brief Returns key's value as T, or fallback if key is absent or the wrong category. */
    template<typename T>
    T get(const std::string& key, T fallback) const {
        auto it = values_.find(key);
        if (it == values_.end()) return fallback;
        return it->second.as<T>(fallback);
    }

    bool contains(const std::string& key) const { return values_.find(key) != values_.end(); }

    /** @brief key's value rendered as text (see EventValue::to_string()), or fallback if key is absent. */
    std::string to_string(const std::string& key, const std::string& fallback = "") const {
        auto it = values_.find(key);
        return it == values_.end() ? fallback : it->second.to_string();
    }

private:
    std::unordered_map<std::string, EventValue> values_;
};

/**
 * @class EventBus
 * @brief Named (object name, signal name) pub/sub, plus a signal-name-only wildcard tier.
 *
 * Every (object name, signal name) pair and every wildcard signal name gets
 * its own Signal<const EventArgs&> under the hood, so EventBus inherits
 * Signal's re-entrancy and lifetime guarantees (safe self-disconnect and
 * self-destroy mid-emit; a Connection outliving the EventBus degrades to
 * "not connected" rather than dangling) — see signal.h.
 *
 * Not thread-safe, matching Signal: on(), on_any(), and emit() on one
 * EventBus must be serialized by the caller.
 *
 * @code
 * EventBus bus;
 * auto conn = bus.on("ButtonPanel", "click", [](const EventArgs&) { ... });
 * bus.emit("ButtonPanel", "click");
 * @endcode
 */
class EventBus {
public:
    using Handler = std::function<void(const EventArgs&)>;

    /** @brief Listens for signal_name emitted specifically as object_name. */
    Connection on(const std::string& object_name, const std::string& signal_name, Handler fn) {
        return per_object_[object_name][signal_name].connect(std::move(fn));
    }

    /** @brief Listens for signal_name emitted as ANY object. */
    Connection on_any(const std::string& signal_name, Handler fn) {
        return wildcard_[signal_name].connect(std::move(fn));
    }

    /**
     * @brief Emits signal_name as object_name with args, notifying both the
     *        object-specific listeners and the signal's wildcard listeners.
     *
     * A no-op if nothing has ever connected for this (object_name,
     * signal_name) pair or this signal_name's wildcard tier — emitting never
     * grows the registry, only on()/on_any() do.
     */
    void emit(const std::string& object_name, const std::string& signal_name, const EventArgs& args = EventArgs()) {
        auto obj_it = per_object_.find(object_name);
        if (obj_it != per_object_.end()) {
            auto sig_it = obj_it->second.find(signal_name);
            if (sig_it != obj_it->second.end()) sig_it->second.emit(args);
        }
        auto wild_it = wildcard_.find(signal_name);
        if (wild_it != wildcard_.end()) wild_it->second.emit(args);
    }

private:
    std::unordered_map<std::string, std::unordered_map<std::string, Signal<const EventArgs&>>> per_object_;
    std::unordered_map<std::string, Signal<const EventArgs&>> wildcard_;
};

}  // namespace event
}  // namespace coopa

#endif  // COOPA_EVENT_EVENT_BUS_H
