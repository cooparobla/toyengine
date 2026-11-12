/**
 * @file input.h
 * @brief The single owner of all keyboard/mouse state — the type every
 * consumer of this library talks to instead of a windowing backend.
 *
 * Data flows IN by push: a backend (gfxcoopa's presentation::Window, wired to
 * GLFW callbacks) calls push_key()/push_cursor_position()/... as OS events
 * arrive, and Input derives every edge, delta, and held-duration from that
 * stream in one place. Data flows OUT by query (key_down(), cursor_delta(),
 * ...) and by control (set_cursor_mode(), ...), the latter forwarded to an
 * attached IInputBackend if one exists.
 *
 * With no backend attached, Input is still a fully functional object — begin_frame()
 * plus a handful of push_*() calls is exactly how it's unit-tested headless
 * (see libcoopa's own test.cpp), and it's also how a mock input source (replay,
 * scripted UI test) would drive one.
 */

#ifndef COOPA_INPUT_INPUT_H
#define COOPA_INPUT_INPUT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/input/input_backend.h>
#include <coopa/input/keys.h>

namespace coopa {
namespace input {

/**
 * @class Input
 * @brief Complete per-frame keyboard/mouse state: level, edges, deltas, held
 * time, discrete events, modifiers, and cursor/clipboard control.
 *
 * Frame contract: a backend calls begin_frame(dt) once, then poll()s the OS
 * (which calls the push_*() methods zero or more times as events arrive),
 * every frame, in that order — see gfxcoopa/presentation/window.h's
 * new_frame()/poll_events(). "Down" state persists across frames; "pressed"/
 * "released" edges and the per-frame event/char/scroll/delta accumulators are
 * all cleared by begin_frame() and rebuilt from that frame's pushes only.
 */
class Input {
public:
    // --- Backend attachment ---

    /// @brief Attaches the backend that set_cursor_*()/set_clipboard_text()
    /// forward to. Pass nullptr to detach (state is then cached, not applied).
    void set_backend(IInputBackend* backend) { backend_ = backend; }
    IInputBackend* backend() const { return backend_; }

    // --- Frame lifecycle: called by the backend only ---

    /// @brief Clears this frame's edges/events/deltas and accumulates
    /// held-time for whatever is still down, using the PREVIOUS frame's
    /// duration (the only one known at the point poll() calls this).
    void begin_frame(float dt) {
        for (std::size_t i = 0; i < kKeyCount; ++i) {
            key_pressed_[i] = false;
            key_released_[i] = false;
            if (key_down_[i]) key_held_time_[i] += dt;
        }
        for (std::size_t i = 0; i < kButtonCount; ++i) {
            button_pressed_[i] = false;
            button_released_[i] = false;
            if (button_down_[i]) button_held_time_[i] += dt;
        }
        chars_.clear();
        key_events_.clear();
        button_events_.clear();
        scroll_delta_ = glm::vec2(0.0f);
        cursor_delta_ = glm::vec2(0.0f);
    }

    /// @brief Records a keyboard transition. Repeats update key_events() but
    /// leave down/pressed/released untouched — the key was already down.
    ///
    /// A modifier key's own bit in the level-triggered mods() state comes from
    /// the transition rather than from the event's mask: X11 — and so GLFW,
    /// which passes the mask through unchanged — stamps a key event with the
    /// modifier state from *before* it, so a Control press carries no Control
    /// bit and a Control release still carries one. Latching the mask verbatim
    /// leaves mods() reporting Control held until the next key or button
    /// event, long after the user let go. The event's own mods stay exactly as
    /// the platform reported them: that is what a chord like Ctrl+Z matches
    /// on, and there the pre-event state is the correct one.
    void push_key(Key key, int scancode, KeyAction action, Mods mods) {
        const Mods own = mod_bit_of(key);
        mods_ = action == KeyAction::Release ? mods & ~own : mods | own;
        key_events_.push_back(KeyEvent{key, scancode, action, mods});
        std::size_t i = index_of(key);
        if (i == kInvalid) return;
        if (action == KeyAction::Press) {
            if (!key_down_[i]) {
                key_pressed_[i] = true;
                key_held_time_[i] = 0.0f;
            }
            key_down_[i] = true;
        } else if (action == KeyAction::Release) {
            if (key_down_[i]) key_released_[i] = true;
            key_down_[i] = false;
            key_held_time_[i] = 0.0f;
        }
    }

    /// @brief Records a printable-text codepoint (UTF-32), independent of push_key().
    void push_char(uint32_t codepoint) { chars_.push_back(codepoint); }

    /// @brief Records a mouse button transition. Event-driven, so a press and
    /// release inside one frame both register — both edges fire, where
    /// level-triggered polling would miss the pair entirely.
    void push_mouse_button(MouseButton button, KeyAction action, Mods mods) {
        mods_ = mods;
        button_events_.push_back(MouseButtonEvent{button, action, mods});
        std::size_t i = index_of(button);
        if (i == kInvalid) return;
        if (action == KeyAction::Press) {
            if (!button_down_[i]) {
                button_pressed_[i] = true;
                button_held_time_[i] = 0.0f;
            }
            button_down_[i] = true;
        } else if (action == KeyAction::Release) {
            if (button_down_[i]) button_released_[i] = true;
            button_down_[i] = false;
            button_held_time_[i] = 0.0f;
        }
    }

    /// @brief Records the cursor's absolute position, in window coordinates
    /// (+Y down). cursor_delta() accumulates the change across however many
    /// times this is called within one frame.
    ///
    /// The very first call ever, and the first call after set_cursor_mode()
    /// changes mode, reports a zero delta instead of jumping — GLFW's virtual
    /// cursor position is undefined/arbitrary on the frame CursorMode::Disabled
    /// is first applied, so treating that one sample as a fresh baseline
    /// (rather than a real motion) avoids a one-frame camera snap.
    void push_cursor_position(double x, double y) {
        glm::vec2 pos(static_cast<float>(x), static_cast<float>(y));
        if (cursor_valid_) {
            cursor_delta_ += pos - cursor_position_;
        } else {
            cursor_valid_ = true;
        }
        cursor_position_ = pos;
    }

    /// @brief Accumulates a scroll wheel delta, in wheel notches.
    void push_scroll(double x, double y) {
        scroll_delta_ += glm::vec2(static_cast<float>(x), static_cast<float>(y));
    }

    /// @brief Records the window's OS focus state. Losing focus releases
    /// every held key/button — the OS is not guaranteed to deliver their
    /// release events once focus is gone, so without this a key held during
    /// an alt-tab would otherwise read as stuck down indefinitely.
    void push_focus(bool focused) {
        focused_ = focused;
        if (!focused) release_all();
    }

    /// @brief Records whether the cursor is currently inside the window's client area.
    void push_cursor_enter(bool entered) { cursor_inside_ = entered; }

    /// @brief Forces every key and mouse button to released, firing this
    /// frame's release edge for anything that was down. See push_focus().
    void release_all() {
        for (std::size_t i = 0; i < kKeyCount; ++i) {
            if (key_down_[i]) key_released_[i] = true;
            key_down_[i] = false;
            key_held_time_[i] = 0.0f;
        }
        for (std::size_t i = 0; i < kButtonCount; ++i) {
            if (button_down_[i]) button_released_[i] = true;
            button_down_[i] = false;
            button_held_time_[i] = 0.0f;
        }
        mods_ = Mods::None;
    }

    // --- Keyboard queries ---

    /// @brief True while `key` is held down (level-triggered).
    bool key_down(Key key) const { std::size_t i = index_of(key); return i != kInvalid && key_down_[i]; }
    /// @brief True on the exact frame `key` transitioned to down.
    bool key_pressed(Key key) const { std::size_t i = index_of(key); return i != kInvalid && key_pressed_[i]; }
    /// @brief True on the exact frame `key` transitioned to up.
    bool key_released(Key key) const { std::size_t i = index_of(key); return i != kInvalid && key_released_[i]; }
    /// @brief Seconds `key` has been continuously held; 0 if it isn't down.
    float key_held_time(Key key) const { std::size_t i = index_of(key); return i != kInvalid ? key_held_time_[i] : 0.0f; }
    /// @brief True if any key at all is currently down.
    bool any_key_down() const {
        for (std::size_t i = 0; i < kKeyCount; ++i) if (key_down_[i]) return true;
        return false;
    }
    /// @brief True if any key at all was pressed this frame.
    bool any_key_pressed() const {
        for (std::size_t i = 0; i < kKeyCount; ++i) if (key_pressed_[i]) return true;
        return false;
    }

    // --- Mouse queries ---

    /// @brief True while `button` is held down (level-triggered).
    bool button_down(MouseButton button) const { std::size_t i = index_of(button); return i != kInvalid && button_down_[i]; }
    /// @brief True on the exact frame `button` transitioned to down.
    bool button_pressed(MouseButton button) const { std::size_t i = index_of(button); return i != kInvalid && button_pressed_[i]; }
    /// @brief True on the exact frame `button` transitioned to up.
    bool button_released(MouseButton button) const { std::size_t i = index_of(button); return i != kInvalid && button_released_[i]; }
    /// @brief Seconds `button` has been continuously held; 0 if it isn't down.
    float button_held_time(MouseButton button) const { std::size_t i = index_of(button); return i != kInvalid ? button_held_time_[i] : 0.0f; }

    /// @brief The modifier mask active as of the most recent key or button event.
    Mods mods() const { return mods_; }
    /// @brief True if every bit of `m` is currently held (see has()).
    bool mod_down(Mods m) const { return has(mods_, m); }

    /// @brief Cursor position in window coordinates (+Y down), as of the last push_cursor_position().
    const glm::vec2& cursor_position() const { return cursor_position_; }
    /// @brief Cursor motion accumulated since the last begin_frame().
    const glm::vec2& cursor_delta() const { return cursor_delta_; }
    /// @brief Scroll wheel delta accumulated since the last begin_frame().
    const glm::vec2& scroll_delta() const { return scroll_delta_; }

    /// @brief UTF-32 codepoints typed since the last begin_frame().
    const std::vector<uint32_t>& chars() const { return chars_; }
    /// @brief Discrete key press/release/repeat events since the last begin_frame().
    const std::vector<KeyEvent>& key_events() const { return key_events_; }
    /// @brief Discrete mouse button press/release events since the last begin_frame().
    const std::vector<MouseButtonEvent>& button_events() const { return button_events_; }

    /// @brief True if the window currently has OS input focus.
    bool focused() const { return focused_; }
    /// @brief True if the cursor is currently inside the window's client area.
    bool cursor_inside() const { return cursor_inside_; }

    // --- Control: forwarded to the attached backend, always cached locally
    // so it reads back correctly even with no backend attached (headless). ---

    /// @brief Sets the mouse cursor's shape.
    void set_cursor_shape(CursorShape shape) {
        cursor_shape_ = shape;
        if (backend_) backend_->set_cursor_shape(shape);
    }
    /// @brief The shape last set via set_cursor_shape() (Arrow by default).
    CursorShape cursor_shape() const { return cursor_shape_; }

    /// @brief Sets the cursor's visibility/capture mode. Re-arms the
    /// first-delta suppression described on push_cursor_position(), since a
    /// mode change (in particular entering Disabled) is exactly when a
    /// backend's next reported position is liable to jump.
    void set_cursor_mode(CursorMode mode) {
        cursor_mode_ = mode;
        cursor_valid_ = false;
        if (backend_) backend_->set_cursor_mode(mode);
    }
    /// @brief The mode last set via set_cursor_mode() (Normal by default).
    CursorMode cursor_mode() const { return cursor_mode_; }

    /// @brief Warps the cursor to (x, y) in window coordinates. This call
    /// itself is the new position baseline, not a jump to suppress -- unlike
    /// set_cursor_mode(), the next cursor_delta() is NOT zeroed.
    void set_cursor_position(double x, double y) {
        cursor_position_ = glm::vec2(static_cast<float>(x), static_cast<float>(y));
        cursor_valid_ = true;
        if (backend_) backend_->set_cursor_position(x, y);
    }

    /// @brief Sets the system clipboard's text contents.
    void set_clipboard_text(const std::string& text) {
        clipboard_text_ = text;
        if (backend_) backend_->set_clipboard_text(text);
    }
    /// @brief Returns the system clipboard's text -- from the backend if one
    /// is attached, else the last value cached by set_clipboard_text().
    std::string clipboard_text() const {
        return backend_ ? backend_->clipboard_text() : clipboard_text_;
    }

private:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t kButtonCount = static_cast<std::size_t>(MouseButton::Count);
    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    static std::size_t index_of(Key key) {
        if (key == Key::Unknown) return kInvalid;
        std::size_t i = static_cast<std::size_t>(key);
        return i < kKeyCount ? i : kInvalid;
    }
    static std::size_t index_of(MouseButton button) {
        std::size_t i = static_cast<std::size_t>(button);
        return i < kButtonCount ? i : kInvalid;
    }

    /// @brief The modifier bit `key` is, or Mods::None for every other key.
    static constexpr Mods mod_bit_of(Key key) {
        switch (key) {
            case Key::LeftShift:   case Key::RightShift:   return Mods::Shift;
            case Key::LeftControl: case Key::RightControl: return Mods::Control;
            case Key::LeftAlt:     case Key::RightAlt:     return Mods::Alt;
            case Key::LeftSuper:   case Key::RightSuper:   return Mods::Super;
            default: return Mods::None;
        }
    }

    IInputBackend* backend_ = nullptr;

    bool  key_down_[kKeyCount] = {};
    bool  key_pressed_[kKeyCount] = {};
    bool  key_released_[kKeyCount] = {};
    float key_held_time_[kKeyCount] = {};

    bool  button_down_[kButtonCount] = {};
    bool  button_pressed_[kButtonCount] = {};
    bool  button_released_[kButtonCount] = {};
    float button_held_time_[kButtonCount] = {};

    Mods mods_ = Mods::None;

    glm::vec2 cursor_position_{0.0f};
    glm::vec2 cursor_delta_{0.0f};
    glm::vec2 scroll_delta_{0.0f};
    bool      cursor_valid_ = false; ///< False until the first push_cursor_position() (or after a mode change).

    std::vector<uint32_t>              chars_;
    std::vector<KeyEvent>              key_events_;
    std::vector<MouseButtonEvent>      button_events_;

    bool focused_ = true;
    bool cursor_inside_ = true;

    CursorShape cursor_shape_ = CursorShape::Arrow;
    CursorMode  cursor_mode_ = CursorMode::Normal;
    std::string clipboard_text_;
};

} // namespace input
} // namespace coopa

#endif // COOPA_INPUT_INPUT_H
