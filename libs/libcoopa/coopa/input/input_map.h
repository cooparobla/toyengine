/**
 * @file input_map.h
 * @brief Named action/axis bindings over keys and mouse buttons, resolved
 * against an Input.
 *
 * Covers key and mouse-button bindings, modifier chords, edge queries, axis
 * pairs, and 2D vector bindings. Lives alongside keys.h, independent of any
 * windowing library; see coopa/input/README.md.
 */

#ifndef COOPA_INPUT_INPUT_MAP_H
#define COOPA_INPUT_INPUT_MAP_H

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/input/input.h>
#include <coopa/input/keys.h>

namespace coopa {
namespace input {

/// @brief A predicate answering "is this key currently held down?". Exists
/// only so pre-Input call sites and tests can query an InputMap with a mock
/// key set instead of a real Input -- see is_down(action, const KeyState&).
using KeyState = std::function<bool(Key)>;

/**
 * @class InputMap
 * @brief Maps named actions to one or more keys/mouse buttons (optionally
 * requiring a modifier chord), and named axes/vectors to key pairs.
 */
class InputMap {
public:
    /// @brief One binding: a key or a mouse button, plus an optional required chord.
    struct Binding {
        enum class Source { Key, MouseButton };
        Source      source = Source::Key;
        Key         key = Key::Unknown;
        MouseButton button = MouseButton::Count; ///< Only meaningful when source == MouseButton.
        Mods        required_mods = Mods::None;  ///< None means "no chord required".
    };

    /// @brief Binds a key to an action, optionally requiring `required_mods`
    /// to be held too (e.g. bind("save", Key::S, Mods::Control)). An action
    /// may have multiple bindings; is_down()/is_pressed() return true if any
    /// one of them currently matches.
    void bind(std::string_view action, Key key, Mods required_mods = Mods::None) {
        Binding b;
        b.source = Binding::Source::Key;
        b.key = key;
        b.required_mods = required_mods;
        bindings_[std::string(action)].push_back(b);
    }

    /// @brief Binds a mouse button to an action. See the Key overload.
    void bind(std::string_view action, MouseButton button, Mods required_mods = Mods::None) {
        Binding b;
        b.source = Binding::Source::MouseButton;
        b.button = button;
        b.required_mods = required_mods;
        bindings_[std::string(action)].push_back(b);
    }

    /// @brief Removes every binding for `action`. Leaves `action` unbound
    /// afterward, same as if bind() had never been called for it.
    void unbind(std::string_view action) { bindings_.erase(std::string(action)); }

    /// @brief Returns the bindings currently registered for `action` (empty if none).
    const std::vector<Binding>& bindings(std::string_view action) const {
        static const std::vector<Binding> empty;
        auto it = bindings_.find(std::string(action));
        return it != bindings_.end() ? it->second : empty;
    }

    /// @brief True if any binding for `action` is currently down (chord included).
    bool is_down(std::string_view action, const Input& input) const {
        for (const Binding& b : bindings(action)) {
            if (matches_(b, input) && down_(b, input)) return true;
        }
        return false;
    }

    /// @brief True if any binding for `action` transitioned to down this frame.
    bool is_pressed(std::string_view action, const Input& input) const {
        for (const Binding& b : bindings(action)) {
            if (matches_(b, input) && pressed_(b, input)) return true;
        }
        return false;
    }

    /// @brief True if any binding for `action` transitioned to up this frame.
    bool is_released(std::string_view action, const Input& input) const {
        for (const Binding& b : bindings(action)) {
            if (matches_(b, input) && released_(b, input)) return true;
        }
        return false;
    }

    /// @brief True if any key bound to `action` is currently down, per
    /// `is_key_pressed`. Predicate form, for callers that want to query
    /// without constructing a real Input (e.g. a mock over a fake key set);
    /// only Key bindings participate, since a KeyState has no notion of
    /// mouse buttons or modifiers.
    bool is_down(std::string_view action, const KeyState& is_key_pressed) const {
        for (const Binding& b : bindings(action)) {
            if (b.source == Binding::Source::Key && is_key_pressed(b.key)) return true;
        }
        return false;
    }

    /// @brief Binds a two-key axis (e.g. "move_x" -> D/A), read via axis().
    void bind_axis(std::string_view axis, Key positive, Key negative) {
        axes_[std::string(axis)] = AxisBinding{positive, negative};
    }

    /// @brief Returns -1, 0, or +1 depending on which of the axis's two keys
    /// (if any) is held. If both are held, they cancel to 0.
    float axis(std::string_view axis, const Input& input) const {
        auto it = axes_.find(std::string(axis));
        if (it == axes_.end()) return 0.0f;
        float value = 0.0f;
        if (input.key_down(it->second.positive)) value += 1.0f;
        if (input.key_down(it->second.negative)) value -= 1.0f;
        return value;
    }

    /// @brief Predicate form of axis() -- see is_down(action, const KeyState&).
    float axis(std::string_view axis, const KeyState& is_key_pressed) const {
        auto it = axes_.find(std::string(axis));
        if (it == axes_.end()) return 0.0f;
        float value = 0.0f;
        if (is_key_pressed(it->second.positive)) value += 1.0f;
        if (is_key_pressed(it->second.negative)) value -= 1.0f;
        return value;
    }

    /// @brief Binds a four-key 2D vector (e.g. WASD movement), read via vector().
    void bind_vector(std::string_view name, Key pos_x, Key neg_x, Key pos_y, Key neg_y) {
        vectors_[std::string(name)] = VectorBinding{pos_x, neg_x, pos_y, neg_y};
    }

    /// @brief Returns the {-1,0,1} x {-1,0,1} vector for `name`'s four keys
    /// (0,0) if unbound, or on either axis if both its keys are held.
    glm::vec2 vector(std::string_view name, const Input& input) const {
        auto it = vectors_.find(std::string(name));
        if (it == vectors_.end()) return glm::vec2(0.0f);
        const VectorBinding& v = it->second;
        return glm::vec2(
            (input.key_down(v.pos_x) ? 1.0f : 0.0f) - (input.key_down(v.neg_x) ? 1.0f : 0.0f),
            (input.key_down(v.pos_y) ? 1.0f : 0.0f) - (input.key_down(v.neg_y) ? 1.0f : 0.0f));
    }

private:
    struct AxisBinding { Key positive; Key negative; };
    struct VectorBinding { Key pos_x; Key neg_x; Key pos_y; Key neg_y; };

    /// @brief True if `b`'s required chord (if any) is currently held.
    static bool matches_(const Binding& b, const Input& input) {
        return has(input.mods(), b.required_mods);
    }
    static bool down_(const Binding& b, const Input& input) {
        return b.source == Binding::Source::Key ? input.key_down(b.key) : input.button_down(b.button);
    }
    static bool pressed_(const Binding& b, const Input& input) {
        return b.source == Binding::Source::Key ? input.key_pressed(b.key) : input.button_pressed(b.button);
    }
    static bool released_(const Binding& b, const Input& input) {
        return b.source == Binding::Source::Key ? input.key_released(b.key) : input.button_released(b.button);
    }

    std::unordered_map<std::string, std::vector<Binding>> bindings_;
    std::unordered_map<std::string, AxisBinding>           axes_;
    std::unordered_map<std::string, VectorBinding>         vectors_;
};

} // namespace input
} // namespace coopa

#endif // COOPA_INPUT_INPUT_MAP_H
