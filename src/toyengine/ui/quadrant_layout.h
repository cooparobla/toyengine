/**
 * @file quadrant_layout.h
 * @brief Pure layout math for a workspace of up to four panels sharing the four screen
 *        quadrants, each growing into any adjacent quadrant left empty.
 *
 * No `Context`, no component, no scene -- a panel's box is a function of the AREA, which of
 * the four slots are currently open, and where the two dividers sit, nothing else.
 * `toy::ui::QuadrantDock` is the one caller; this file is what makes its growth rule testable
 * with no GPU and no font.
 */

#ifndef TOYENGINE_UI_QUADRANT_LAYOUT_H
#define TOYENGINE_UI_QUADRANT_LAYOUT_H

#include <array>

#include <uicoopa/immediate/imm.h>

namespace toy::ui {

/** @brief The four screen quadrants. Also indexes `std::array<T, 4>` throughout this file. */
enum class Slot { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3, Count = 4 };

/**
 * @brief Where the two dividers sit, as fractions of the area: x is the vertical divider's
 *        distance from the left edge, y the horizontal divider's from the top.
 *
 * `{0.5f, 0.5f}` is four equal quadrants. Dragging a divider moves one component, which is
 * what lets one panel be made much larger than its neighbours without changing which slot
 * anything is in.
 */
struct QuadrantSplit {
    float x = 0.5f;
    float y = 0.5f;
};

/** @brief The closest a divider may come to an edge, so a panel can never be dragged to nothing. */
inline constexpr float kMinSplit = 0.15f;
/** @brief The far-edge counterpart of kMinSplit. */
inline constexpr float kMaxSplit = 0.85f;

/**
 * @brief Boxes for all four quadrants, each panel grown through its adjacent empty slots.
 *
 * `open[i]` says whether Slot `i` holds a panel; a closed slot's returned box is unused by
 * the caller (nothing is drawn there) and is not guaranteed to be anything in particular.
 *
 * The rule, so a mixed arrangement never leaves two panels claiming the same empty quadrant
 * or a quadrant claimed by no one:
 *
 * | Open count | Result |
 * |---|---|
 * | 0 | unused -- nothing is drawn |
 * | 1 | that panel fills the whole area |
 * | 2, same row (TopLeft+TopRight or BottomLeft+BottomRight) | two full-HEIGHT halves, each growing into the empty cell below/above it in its own column |
 * | 2, same column (TopLeft+BottomLeft or TopRight+BottomRight) | two full-WIDTH halves, each growing into the empty cell beside it in its own row |
 * | 2, diagonal (TopLeft+BottomRight or TopRight+BottomLeft) | two full-WIDTH halves, stacked -- each grows into the empty cell in its OWN row |
 * | 3 | the one alone in its column (the other member of that column is the empty slot) spans full height; the other two stay single quadrants |
 * | 4 | four quadrants, unchanged |
 *
 * `split` only moves where the dividers are; it never changes which slots merge, so every row
 * of the table above holds at any divider position.
 */
std::array<coopa::ui::imm::Box, 4> quadrant_layout(const coopa::ui::imm::Box& area,
                                                    const std::array<bool, 4>& open,
                                                    QuadrantSplit split = {});

/**
 * @brief The two dividers' grab rects for an arrangement, or empty boxes where there is no
 *        divider to drag.
 *
 * A divider only exists where the layout actually splits, and only SPANS what it resizes --
 * judged by which PANEL covers each quadrant, so a grown panel still forms a boundary. With
 * three panels open, the right column is one tall panel: the horizontal divider splits the left
 * column alone and stops at the vertical divider, while the VERTICAL divider runs the full
 * height, because the tall panel borders both left-hand quadrants.
 *
 * @param area  The same area passed to quadrant_layout().
 * @param open  Which slots hold a panel.
 * @param split Where the dividers sit.
 * @param grab  Thickness of a divider's grab strip, in the area's units.
 */
struct QuadrantDividers {
    /** @brief Splits a row left-from-right; empty (w or h of 0) when no row is split. */
    coopa::ui::imm::Box vertical{};
    /** @brief Splits a column top-from-bottom; empty when no column is split. */
    coopa::ui::imm::Box horizontal{};
};
QuadrantDividers quadrant_dividers(const coopa::ui::imm::Box& area, const std::array<bool, 4>& open,
                                   QuadrantSplit split = {}, float grab = 10.0f);

/** @brief The Slot whose BASE quadrant (not a grown box) centre is nearest `p` -- where a panel dragged to `p` lands. */
Slot nearest_slot(const coopa::ui::imm::Box& area, glm::vec2 p, QuadrantSplit split = {});

}  // namespace toy::ui

#endif  // TOYENGINE_UI_QUADRANT_LAYOUT_H
