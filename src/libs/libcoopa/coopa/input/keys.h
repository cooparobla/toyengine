/**
 * @file keys.h
 * @brief libcoopa-owned keyboard/mouse vocabulary — free of any windowing
 * library or graphics API.
 *
 * Key/MouseButton values are dense, 0-based, and coopa-defined — deliberately
 * NOT GLFW (or SDL, or platform-native) codes. A backend's own mapping table
 * (e.g. gfxcoopa's internal gfxcoopa/detail/glfw_keys.h) converts to/from
 * these; nothing outside a backend may reach for the underlying library's
 * key/button constants directly. This is what lets consumer code write
 * `input_map.bind("jump", Key::Space)` instead of naming `GLFW_KEY_SPACE`,
 * and what makes the vocabulary itself backend-swappable.
 *
 * This vocabulary lives in libcoopa, not in a graphics package, because it has
 * zero dependency on Vulkan or any windowing library: naming a key should not
 * require a graphics repo as a build dependency. See coopa/input/README.md.
 */

#ifndef COOPA_INPUT_KEYS_H
#define COOPA_INPUT_KEYS_H

#include <cstdint>

namespace coopa {
namespace input {

/**
 * @enum Key
 * @brief A physical/logical keyboard key.
 *
 * Values are dense and 0-based so `size_t(Key::Count)` sizes lookup arrays
 * directly — unlike GLFW's key codes, which are sparse and not usable that
 * way without an offset table.
 */
enum class Key : int16_t {
    Unknown = -1,

    Space = 0, Apostrophe, Comma, Minus, Period, Slash,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Semicolon, Equal,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket, Backslash, RightBracket, GraveAccent,

    Escape, Enter, Tab, Backspace, Insert, Delete,
    Right, Left, Down, Up,
    PageUp, PageDown, Home, End,
    CapsLock, ScrollLock, NumLock, PrintScreen, Pause,

    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24, F25,

    Kp0, Kp1, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9,
    KpDecimal, KpDivide, KpMultiply, KpSubtract, KpAdd, KpEnter, KpEqual,

    LeftShift, LeftControl, LeftAlt, LeftSuper,
    RightShift, RightControl, RightAlt, RightSuper,
    Menu,

    Count, ///< Not a real key. Sizes dense lookup arrays: `size_t(Key::Count)`.
};

/// @brief A mouse button. Dense and 0-based, unlike GLFW's button indices
/// (which happen to also be 0-based, but this keeps the two vocabularies
/// independent so coopa is free to diverge later).
enum class MouseButton : uint8_t {
    Left = 0, Right, Middle,
    Button4, Button5, Button6, Button7, Button8,
    Count, ///< Not a real button. Sizes dense lookup arrays.
};

/// @brief The transition a KeyEvent/MouseButtonEvent represents.
enum class KeyAction : uint8_t {
    Release,
    Press,
    Repeat,
};

/// @brief Modifier-key bitmask, active at the moment of a KeyEvent.
enum class Mods : uint8_t {
    None     = 0,
    Shift    = 1u << 0,
    Control  = 1u << 1,
    Alt      = 1u << 2,
    Super    = 1u << 3,
    CapsLock = 1u << 4,
    NumLock  = 1u << 5,
};

/// @brief Combines two modifier masks.
constexpr Mods operator|(Mods a, Mods b) {
    return static_cast<Mods>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// @brief Merges `b` into `a` in place.
constexpr Mods& operator|=(Mods& a, Mods b) { return a = a | b; }

/// @brief Intersects two modifier masks.
constexpr Mods operator&(Mods a, Mods b) {
    return static_cast<Mods>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/// @brief Complements a modifier mask, for clearing bits (`m & ~Mods::Control`).
constexpr Mods operator~(Mods m) {
    return static_cast<Mods>(static_cast<uint8_t>(~static_cast<uint8_t>(m)));
}

/// @brief True if `set` contains every bit set in `m` (e.g. a Ctrl+Shift
/// chord's `has(mods, Mods::Control | Mods::Shift)`). `m == Mods::None`
/// always returns true, since the empty mask's bits are trivially all
/// present -- this is what lets an InputMap binding's "no chord required"
/// default just be Mods::None rather than a special case.
constexpr bool has(Mods set, Mods m) {
    return (set & m) == m;
}

/// @brief A single discrete keyboard event captured during the last input
/// poll. Unlike level-triggered "is this key down" queries, this captures
/// edges and OS key-repeat.
struct KeyEvent {
    Key       key = Key::Unknown;
    int       scancode = 0;  ///< Platform-specific scancode, passed through unchanged.
    KeyAction action = KeyAction::Release;
    Mods      mods = Mods::None;
};

/// @brief A single discrete mouse button event, the button-side sibling of KeyEvent.
struct MouseButtonEvent {
    MouseButton button = MouseButton::Left;
    KeyAction   action = KeyAction::Release;
    Mods        mods = Mods::None;
};

/// @brief Standard cursor shapes for hover feedback (e.g. text fields, buttons).
enum class CursorShape {
    Arrow,
    IBeam,
    Hand,
    ResizeH,   ///< Left-right resize (a vertical panel border).
    ResizeV,   ///< Up-down resize (a horizontal panel border).
};

/// @brief Visibility/capture behavior of the OS cursor, independent of its shape.
enum class CursorMode {
    Normal,   ///< Cursor visible and unconfined (default).
    Hidden,   ///< Cursor hidden but still confined to the window and reporting real position.
    Disabled, ///< Cursor hidden, unbounded, and re-centered each frame -- for mouse-look.
};

/// @brief A short display name for `key`, e.g. for a keybinding-remap UI.
/// Returns "Unknown" for Key::Unknown, Key::Count, or any other invalid value.
inline const char* key_name(Key key) {
    switch (key) {
        case Key::Space: return "Space";
        case Key::Apostrophe: return "'";
        case Key::Comma: return ",";
        case Key::Minus: return "-";
        case Key::Period: return ".";
        case Key::Slash: return "/";
        case Key::Num0: return "0"; case Key::Num1: return "1"; case Key::Num2: return "2";
        case Key::Num3: return "3"; case Key::Num4: return "4"; case Key::Num5: return "5";
        case Key::Num6: return "6"; case Key::Num7: return "7"; case Key::Num8: return "8";
        case Key::Num9: return "9";
        case Key::Semicolon: return ";";
        case Key::Equal: return "=";
        case Key::A: return "A"; case Key::B: return "B"; case Key::C: return "C";
        case Key::D: return "D"; case Key::E: return "E"; case Key::F: return "F";
        case Key::G: return "G"; case Key::H: return "H"; case Key::I: return "I";
        case Key::J: return "J"; case Key::K: return "K"; case Key::L: return "L";
        case Key::M: return "M"; case Key::N: return "N"; case Key::O: return "O";
        case Key::P: return "P"; case Key::Q: return "Q"; case Key::R: return "R";
        case Key::S: return "S"; case Key::T: return "T"; case Key::U: return "U";
        case Key::V: return "V"; case Key::W: return "W"; case Key::X: return "X";
        case Key::Y: return "Y"; case Key::Z: return "Z";
        case Key::LeftBracket: return "["; case Key::Backslash: return "\\";
        case Key::RightBracket: return "]"; case Key::GraveAccent: return "`";
        case Key::Escape: return "Escape"; case Key::Enter: return "Enter";
        case Key::Tab: return "Tab"; case Key::Backspace: return "Backspace";
        case Key::Insert: return "Insert"; case Key::Delete: return "Delete";
        case Key::Right: return "Right"; case Key::Left: return "Left";
        case Key::Down: return "Down"; case Key::Up: return "Up";
        case Key::PageUp: return "PageUp"; case Key::PageDown: return "PageDown";
        case Key::Home: return "Home"; case Key::End: return "End";
        case Key::CapsLock: return "CapsLock"; case Key::ScrollLock: return "ScrollLock";
        case Key::NumLock: return "NumLock"; case Key::PrintScreen: return "PrintScreen";
        case Key::Pause: return "Pause";
        case Key::F1: return "F1"; case Key::F2: return "F2"; case Key::F3: return "F3";
        case Key::F4: return "F4"; case Key::F5: return "F5"; case Key::F6: return "F6";
        case Key::F7: return "F7"; case Key::F8: return "F8"; case Key::F9: return "F9";
        case Key::F10: return "F10"; case Key::F11: return "F11"; case Key::F12: return "F12";
        case Key::F13: return "F13"; case Key::F14: return "F14"; case Key::F15: return "F15";
        case Key::F16: return "F16"; case Key::F17: return "F17"; case Key::F18: return "F18";
        case Key::F19: return "F19"; case Key::F20: return "F20"; case Key::F21: return "F21";
        case Key::F22: return "F22"; case Key::F23: return "F23"; case Key::F24: return "F24";
        case Key::F25: return "F25";
        case Key::Kp0: return "Numpad 0"; case Key::Kp1: return "Numpad 1";
        case Key::Kp2: return "Numpad 2"; case Key::Kp3: return "Numpad 3";
        case Key::Kp4: return "Numpad 4"; case Key::Kp5: return "Numpad 5";
        case Key::Kp6: return "Numpad 6"; case Key::Kp7: return "Numpad 7";
        case Key::Kp8: return "Numpad 8"; case Key::Kp9: return "Numpad 9";
        case Key::KpDecimal: return "Numpad ."; case Key::KpDivide: return "Numpad /";
        case Key::KpMultiply: return "Numpad *"; case Key::KpSubtract: return "Numpad -";
        case Key::KpAdd: return "Numpad +"; case Key::KpEnter: return "Numpad Enter";
        case Key::KpEqual: return "Numpad =";
        case Key::LeftShift: return "Left Shift"; case Key::LeftControl: return "Left Control";
        case Key::LeftAlt: return "Left Alt"; case Key::LeftSuper: return "Left Super";
        case Key::RightShift: return "Right Shift"; case Key::RightControl: return "Right Control";
        case Key::RightAlt: return "Right Alt"; case Key::RightSuper: return "Right Super";
        case Key::Menu: return "Menu";
        case Key::Unknown: default: return "Unknown";
    }
}

} // namespace input
} // namespace coopa

#endif // COOPA_INPUT_KEYS_H
