#pragma once

/**
 * @file tool_strip.h
 * @brief A row of icon buttons laid out at a fixed pitch: the viewport's tool strip, a
 *        panel's dense toolbar, the timeline's transport.
 *
 * Every strip in the editor had its own lambda for "next button box, draw an icon button
 * there, advance", each with a slightly different advance rule and only some of them
 * recording where the button landed. One strip means one pitch, and `button()` always
 * registers its rect with the editor, so a test can click anything a strip draws.
 */

#include <string>

#include <uicoopa/immediate/imm.h>

#include "editor/app/ui/metrics.h"

namespace toy {
namespace editor {

class EditorApp;

namespace ui {

/**
 * @brief Walks a bar left to right (or right to left), handing out equal square boxes.
 *
 * @code
 * ui::ToolStrip s{bar, ui::k_tool_small, 2.0f, false, &app_, "weather:"};
 * if (s.button(ctx, "add", I::Plus, "Add a condition")) add();
 * if (s.button(ctx, "up", I::ArrowUp, "Move Up", false, sel > 0)) move(-1);
 * @endcode
 */
struct ToolStrip {
    imm::Box bar;                      ///< The region the buttons sit in; its height sets theirs.
    float size = k_tool_small;         ///< One button's side.
    float gap = 2.0f;
    bool rtl = false;                  ///< Pack from the bar's right edge instead of its left.
    EditorApp* app = nullptr;          ///< Where rects are registered; null in a test.
    std::string rect_prefix;           ///< Prepended to each button's id for that registration.
    bool framed = false;               ///< Draw each button's frame even when it is not hovered.

    float cursor = 0.0f;               ///< How far along the bar the next box starts.

    /** @brief The next button's box, advancing the cursor past it. */
    imm::Box next();

    /**
     * @brief An icon button at the next box.
     * @param on      Draw it as the active choice.
     * @param enabled A disabled button draws dimmed and does not click.
     * @return True when clicked.
     */
    bool button(imm::Context& ctx, const char* id, imm::Icon icon, const char* tip, bool on = false,
                bool enabled = true);

    /** @brief Leaves a gap of `n` button widths, to group a strip into sections. */
    void skip(float n = 1.0f) { cursor += n * (size + gap); }
};

/**
 * @brief A dim caption inside a header strip, advancing `x` past it.
 *
 * What a mode panel's header puts before each of its fields ("Radius", "Strength"), so the
 * captions sit at the same baseline and spacing in every mode.
 */
void tool_header_label(imm::Context& ctx, const imm::Box& hb, float& x, const char* text);

/**
 * @brief An icon button at `x` in a header strip, advancing `x` past it.
 *
 * The strip form rather than ToolStrip, because a header interleaves buttons with fields and
 * captions that advance the same cursor; ToolStrip owns its cursor and suits a run of buttons.
 */
bool tool_header_button(imm::Context& ctx, const imm::Box& hb, float& x, const char* id, imm::Icon icon,
                        const char* tip, bool on = false);

} // namespace ui
} // namespace editor
} // namespace toy
