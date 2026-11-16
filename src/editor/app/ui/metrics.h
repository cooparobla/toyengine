#pragma once

/**
 * @file metrics.h
 * @brief The editor's chrome sizes: area heights, gaps, radii, toolbar button sizes, and the
 *        limits the workspace splitters clamp to.
 *
 * These are chrome, not widget metrics: a theme tunes what `imm::Style` holds (row height,
 * padding, rounding), while the numbers here describe the window's own frame, which every
 * theme shares. Naming them is what keeps four sibling areas from each picking their own
 * header height, and stops one panel reaching into another for a constant.
 */

#include <algorithm>

#include <uicoopa/immediate/imm.h>

namespace toy {
namespace editor {
namespace ui {

namespace imm = coopa::ui::imm;

inline constexpr float k_topbar_h    = 28.0f;   ///< Menus, the open-asset chip, play controls.
inline constexpr float k_statusbar_h = 24.0f;   ///< Hints, the transient report, scene stats.

/**
 * @brief An area's corner radius -- deliberately larger than `imm::Style::rounding`, because
 *        an area is a region of the window rather than a widget inside one.
 */
inline constexpr float k_area_radius   = 6.0f;
inline constexpr float k_area_gap      = 4.0f;   ///< Between areas; also the splitter's width.
inline constexpr float k_area_header_h = 26.0f;  ///< Every area header, so siblings line up.

inline constexpr float k_tool_size  = 32.0f;   ///< Viewport tool buttons (the left strip).
inline constexpr float k_tool_small = 26.0f;   ///< Dense icon toolbars inside a panel.

// Workspace splitter limits. Each is a floor in pixels and a ceiling as a fraction of the
// window, so a small window keeps every area usable and a large one does not let one area
// swallow the rest.
inline constexpr float k_min_left_w     = 200.0f;
inline constexpr float k_min_right_w    = 260.0f;
inline constexpr float k_min_bottom_h   =  60.0f;
inline constexpr float k_min_outliner_h =  90.0f;
inline constexpr float k_max_left_frac   = 0.35f;
inline constexpr float k_max_right_frac  = 0.40f;
inline constexpr float k_max_bottom_frac = 0.50f;
/** @brief The outliner's ceiling: whatever the window leaves Properties. */
inline constexpr float k_outliner_slack = 160.0f;

inline constexpr float k_left_split_frac = 0.40f;    ///< Asset list vs the rows/nodes list under it.
inline constexpr float k_left_split_min  = 140.0f;

/** @brief Clamps a splitter value to a pixel floor and a fraction-of-`extent` ceiling. */
inline float clamp_split(float value, float floor_px, float extent, float max_frac) {
    return std::clamp(value, floor_px, std::max(floor_px, extent * max_frac));
}

/** @brief The two halves of a "category list beside a settings pane" body. */
struct ListPane {
    imm::Box list;
    imm::Box pane;
};

/**
 * @brief Splits a modal's body into a fixed-width category list and the pane beside it.
 *
 * Preferences, Project Settings and the World Generator are all this shape; the arithmetic is
 * here so they cannot drift apart by a few pixels each.
 */
inline ListPane list_pane(const imm::Box& body, float list_w = 170.0f, float gap = 6.0f) {
    const float lw = std::min(list_w, std::max(0.0f, body.w - gap));
    return {{body.x, body.y, lw, body.h},
            {body.x + lw + gap, body.y, std::max(0.0f, body.w - lw - gap), body.h}};
}

} // namespace ui
} // namespace editor
} // namespace toy
