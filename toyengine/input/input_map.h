/**
 * @file input_map.h
 * @brief Named input actions over an arbitrary set of key bindings.
 *
 * Deliberately decoupled from gfxcoopa::presentation::Window (or any input
 * source): is_action_down() takes a key-state predicate rather than a
 * Window&, so the binding/query logic is headlessly testable with a plain
 * lambda mock instead of a real GLFW window. A thin call-site adapter
 * (`[&](int k){ return window.is_key_pressed(k); }`) is all a real app needs.
 *
 * Ported from pixengine/input/input_map.h -- the binding model is engine-
 * agnostic, so it is reused verbatim under toy::input.
 */

#ifndef TOYENGINE_INPUT_INPUT_MAP_H
#define TOYENGINE_INPUT_INPUT_MAP_H

#include <string>
#include <unordered_map>
#include <vector>

namespace toy {
namespace input {

/**
 * @class InputMap
 * @brief A named action maps to one or more key codes; the action is "down"
 *        if any bound key is currently pressed.
 */
class InputMap {
public:
    /// @brief Adds `key` (a GLFW_KEY_* code) to the set that satisfies `action`.
    void bind_key(const std::string& action, int glfw_key) {
        bindings_[action].push_back(glfw_key);
    }

    /// @brief Removes every binding for `action`.
    void unbind(const std::string& action) {
        bindings_.erase(action);
    }

    /// @brief The keys currently bound to `action` (empty if unbound).
    const std::vector<int>& bindings(const std::string& action) const {
        static const std::vector<int> empty;
        auto it = bindings_.find(action);
        return it != bindings_.end() ? it->second : empty;
    }

    /**
     * @brief True if `action` is bound to at least one key and any of them
     *        are currently down, per `is_key_pressed`.
     * @param is_key_pressed Callable taking a GLFW key code, returning bool
     *                       -- typically `[&](int k){ return window.is_key_pressed(k); }`.
     */
    template <typename KeyPressedFn>
    bool is_action_down(const std::string& action, KeyPressedFn&& is_key_pressed) const {
        for (int key : bindings(action)) {
            if (is_key_pressed(key)) return true;
        }
        return false;
    }

private:
    std::unordered_map<std::string, std::vector<int>> bindings_;
};

} // namespace input
} // namespace toy

#endif // TOYENGINE_INPUT_INPUT_MAP_H
