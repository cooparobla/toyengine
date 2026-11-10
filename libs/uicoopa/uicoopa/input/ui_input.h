/**
 * @file ui_input.h
 * @brief Per-frame mouse/keyboard state in canvas space, built on a coopa::input::Input.
 *
 * coopa::input::Input already provides edge detection (pressed/released this
 * frame) — see coopa/input/README.md — so UiInput's only remaining job is
 * converting the window-space cursor position into canvas pixel space (+Y
 * up, scaled by the canvas's scale_factor), the space every
 * RectTransform/Raycaster operates in. This file has no windowing-library
 * dependency at all: it never includes gfxcoopa.
 */

#ifndef UICOOPA_INPUT_UI_INPUT_H
#define UICOOPA_INPUT_UI_INPUT_H

#include <coopa/input/input.h>
#include <coopa/input/keys.h>
#include <uicoopa/layout/rect.h>
#include <glm/glm.hpp>
#include <array>
#include <vector>

namespace coopa {
namespace ui {

/**
 * @class UiInput
 * @brief Converts a coopa::input::Input's per-frame state into canvas-space UI input state.
 *
 * Usage, once per frame, before EventSystem::process():
 * @code
 * window.new_frame(dt);
 * window.poll_events();
 * ui_input.update(window.input(), canvas.root_rect(), canvas.scale_factor());
 * @endcode
 */
class UiInput {
public:
    static constexpr int kMaxButtons = static_cast<int>(coopa::input::MouseButton::Count);

    /**
     * @param input           Source of per-frame input state.
     * @param canvas_root_rect The canvas's root_rect(), used to flip the cursor's Y axis.
     * @param scale_factor    The canvas's scale_factor(), converting window pixels to canvas pixels.
     */
    void update(const coopa::input::Input& input, const Rect& canvas_root_rect, float scale_factor,
                float cursor_scale = 1.0f, glm::vec2 screen_origin = glm::vec2(0.0f), float display_zoom = 1.0f) {
        // cursor_scale converts the platform's cursor units into the framebuffer pixels the
        // canvas was sized from (2 on a Retina display, where GLFW reports screen points).
        // screen_origin is where the canvas's top-left sits in those pixels and display_zoom how
        // much it is magnified there -- (0, 0) and 1 except for a canvas placed inside part of
        // the target (CanvasComponent::set_screen_origin() / set_display_zoom()).
        glm::vec2 wpos = (input.cursor_position() * cursor_scale - screen_origin) / (display_zoom > 0.0f ? display_zoom : 1.0f);
        float sf = scale_factor > 0.0f ? scale_factor : 1.0f;

        // Window coordinates are +Y down from the top-left; canvas space is +Y up from the
        // bottom-left, so the vertical axis both flips and rescales.
        glm::vec2 canvas_pos(
            wpos.x / sf,
            canvas_root_rect.size().y - wpos.y / sf);

        update_at(input, canvas_pos);
    }

    /**
     * @brief Same per-frame state as update(), but with the canvas-space cursor position
     *        supplied directly instead of derived from a screen-space scale factor.
     *
     * This is the world-space canvas path: there, the cursor reaches canvas space through a
     * ray/plane intersection against the canvas's 3D placement (see
     * CanvasComponent::ray_to_canvas()), which has no scale_factor to divide by. update()
     * above is just "compute the screen-space cursor, then call this", so the
     * button/scroll/character/key-event copying below exists exactly once.
     *
     * @param input      Source of per-frame input state.
     * @param canvas_pos Cursor position in canvas pixels, +Y up from the bottom-left. A
     *                   caller with no hit to report should pass a point far outside the
     *                   canvas rather than (0, 0), which is a real point *inside* it.
     */
    void update_at(const coopa::input::Input& input, glm::vec2 canvas_pos) {
        delta_ = canvas_pos - position_;
        position_ = canvas_pos;

        for (int b = 0; b < kMaxButtons; ++b) {
            auto button = static_cast<coopa::input::MouseButton>(b);
            buttons_[b] = input.button_down(button);
            pressed_this_frame_[b] = input.button_pressed(button);
            released_this_frame_[b] = input.button_released(button);
        }

        scroll_ = input.scroll_delta();
        char_input_ = input.chars();
        key_events_ = input.key_events();
        mods_ = input.mods();
        keys_down_.fill(false);
        for (std::size_t k = 0; k < keys_down_.size(); ++k) {
            keys_down_[k] = input.key_down(static_cast<coopa::input::Key>(k));
        }
        source_ = &input;
    }

    /**
     * @brief A frame with no input at all: the cursor parked at `canvas_pos` (pass a point
     *        far outside the canvas), no buttons, keys, characters or scroll.
     *
     * For a canvas that is drawn but must not react -- an editor previewing a HUD it is
     * laying out -- so hover and press state drain the same way they would if the pointer
     * simply left.
     */
    void update_idle(glm::vec2 canvas_pos) {
        delta_ = canvas_pos - position_;
        position_ = canvas_pos;
        for (int b = 0; b < kMaxButtons; ++b) {
            buttons_[b] = false;
            pressed_this_frame_[b] = false;
            released_this_frame_[b] = false;
        }
        scroll_ = glm::vec2(0.0f);
        char_input_.clear();
        key_events_.clear();
        mods_ = coopa::input::Mods::None;
        keys_down_.fill(false);
    }

    /** @brief Modifier keys held as of the last update (from the last key/button event). */
    coopa::input::Mods mods() const { return mods_; }

    /** @brief Level-triggered key state as of the last update. */
    bool is_key_down(coopa::input::Key key) const {
        const auto i = static_cast<std::size_t>(key);
        return key != coopa::input::Key::Unknown && i < keys_down_.size() && keys_down_[i];
    }

    /** @brief The Input this was last updated from (clipboard access), or null. */
    const coopa::input::Input* source() const { return source_; }

    const glm::vec2& position() const { return position_; }
    const glm::vec2& delta() const { return delta_; }
    const glm::vec2& scroll_delta() const { return scroll_; }

    bool is_button_down(int button) const {
        return button >= 0 && button < kMaxButtons && buttons_[button];
    }
    bool is_button_pressed(int button) const {
        return button >= 0 && button < kMaxButtons && pressed_this_frame_[button];
    }
    bool is_button_released(int button) const {
        return button >= 0 && button < kMaxButtons && released_this_frame_[button];
    }

    const std::vector<unsigned int>& char_input() const { return char_input_; }
    const std::vector<coopa::input::KeyEvent>& key_events() const { return key_events_; }

private:
    glm::vec2 position_{0.0f};
    glm::vec2 delta_{0.0f};
    glm::vec2 scroll_{0.0f};
    bool buttons_[kMaxButtons] = {};
    bool pressed_this_frame_[kMaxButtons] = {};
    bool released_this_frame_[kMaxButtons] = {};
    std::vector<unsigned int> char_input_;
    std::vector<coopa::input::KeyEvent> key_events_;
    coopa::input::Mods mods_ = coopa::input::Mods::None;
    std::array<bool, static_cast<std::size_t>(coopa::input::Key::Count)> keys_down_{};
    const coopa::input::Input* source_ = nullptr;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_INPUT_UI_INPUT_H
