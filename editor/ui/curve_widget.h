/**
 * @file curve_widget.h
 * @brief The curve and gradient editors as immediate-mode widgets: a box you drag points
 *        around in, and a colour ramp you drag stops along.
 *
 * Both are thin: every decision about where a point goes lives in curve_math.h, which is
 * unit-tested on its own. These two functions draw what that says and turn clicks into calls.
 * They live in the editor rather than in uicoopa because the maths and the YAML shape are the
 * editor's, not the UI library's.
 */

#ifndef TOYEDITOR_UI_CURVE_WIDGET_H
#define TOYEDITOR_UI_CURVE_WIDGET_H

#include "curve_math.h"

#include <uicoopa/immediate/imm.h>

#include <string_view>
#include <vector>

namespace toy::editor::ui {

namespace imm = coopa::ui::imm;

/** @brief What a curve or gradient edit did this frame. */
struct ShapeEdit {
    bool changed = false;    ///< The points were modified.
    bool dragging = false;   ///< A point is still being dragged: merge undo steps.
    bool finished = false;   ///< A drag ended this frame.
    int  selected = -1;      ///< Gradient: the stop the colour row below should edit.
};

/**
 * @brief A curve over the unit square: drag a point to move it, click empty space to add one,
 *        right-click a point to remove it.
 *
 * @param height Row height in pixels; the widget takes the full available width.
 */
ShapeEdit curve_edit(imm::Context& ctx, std::string_view id, std::vector<glm::vec2>& points, float height = 90.0f);

/**
 * @brief A colour ramp: drag a stop along it, click the bar to add one, right-click a stop to
 *        remove it. Clicking a stop selects it, and the selection survives between frames.
 *
 * Editing the selected stop's colour is the caller's job -- it draws a colour row beneath,
 * reading ShapeEdit::selected -- so the picker and its undo step work exactly as every other
 * colour field's do.
 */
ShapeEdit gradient_edit(imm::Context& ctx, std::string_view id, std::vector<ColorStop>& stops, float height = 26.0f);

} // namespace toy::editor::ui

#endif // TOYEDITOR_UI_CURVE_WIDGET_H
