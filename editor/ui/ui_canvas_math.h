/**
 * @file ui_canvas_math.h
 * @brief The UI designer's geometry, with no GPU and no UI: mapping between the editor's
 *        window and a canvas, picking, and the rect edits the gizmo and the inspector make.
 *
 * Two spaces meet here:
 *   - the editor's (imm) space: window pixels / ui scale, +Y DOWN from the top-left;
 *   - canvas space: canvas pixels, +Y UP from the bottom-left (uicoopa's RectTransform space).
 * UiView holds the preview frame that relates them. Every rect edit is expressed on a
 * RectParams (anchors, pivot, anchored position, size delta) against the parent's resolved
 * rect -- the same model uicoopa's resolve_rect() lays out with, so what the gizmo writes is
 * exactly what the game shows.
 */

#ifndef TOYEDITOR_UI_UI_CANVAS_MATH_H
#define TOYEDITOR_UI_UI_CANVAS_MATH_H

#include "../core/yaml_util.h"

#include <uicoopa/layout/rect.h>
#include <uicoopa/layout/rect_transform.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace toy::editor::ui {

using coopa::ui::Rect;
using coopa::ui::RectParams;

/** @brief The preview frame: where the canvas sits in the editor and at what magnification. */
struct UiView {
    glm::vec2 origin{0.0f};        ///< Frame top-left, editor (imm) pixels.
    float scale = 1.0f;            ///< Editor pixels per canvas pixel.
    glm::vec2 canvas_size{1.0f};   ///< The canvas's root rect size, canvas pixels.

    glm::vec2 to_canvas(glm::vec2 p) const;
    glm::vec2 to_editor(glm::vec2 c) const { return {origin.x + c.x * scale, origin.y + (canvas_size.y - c.y) * scale}; }
    /** @brief A canvas-space delta (Y up) for an editor-space delta (Y down). */
    glm::vec2 delta_to_canvas(glm::vec2 d) const;
    /** @brief A canvas rect as an editor box (x, y = top-left). */
    std::array<float, 4> box_of(const Rect& r) const;
};

/** @brief Point-in-convex-quad (corners in order, either winding). */
bool point_in_quad(const std::array<glm::vec2, 4>& q, glm::vec2 p);

// =====================================================================================
// Rect edits -- each keeps whatever it is not changing VISUALLY fixed.
// =====================================================================================

/** @brief The resolved rect's offsets from its anchor rect: min = (left, bottom), max = (right, top). */
void offsets_of(const Rect& parent, const RectParams& p, glm::vec2& off_min, glm::vec2& off_max);

/** @brief Re-expresses the rect `r` (canvas space) under the anchors/pivot already in `p`. */
void set_rect(const Rect& parent, RectParams& p, const Rect& r);

/** @brief Moves the rect by `delta` canvas pixels. */
inline void move_by(RectParams& p, glm::vec2 delta) { p.anchored_position += delta; }

/**
 * @brief Resize from a handle. `hx`/`hy` say which edge moves: -1 the min (left/bottom)
 *        edge, +1 the max edge, 0 neither. The opposite edge stays put; `symmetric` moves
 *        both about the centre; `keep_aspect` holds the starting width:height ratio.
 * @param start The rect when the drag began (canvas space).
 * @param drag  Total cursor travel since then (canvas pixels, Y up).
 */
Rect resize_rect(const Rect& start, int hx, int hy, glm::vec2 drag, bool symmetric, bool keep_aspect,
                        float min_size = 1.0f);

/** @brief Sets anchors, keeping the rect where it is on screen. */
void set_anchors_keep_rect(const Rect& parent, RectParams& p, glm::vec2 amin, glm::vec2 amax);

/** @brief Sets the pivot, keeping the rect where it is (anchored_position compensates). */
void set_pivot_keep_rect(const Rect& parent, RectParams& p, glm::vec2 pivot);

/** @brief Unity's anchor presets as anchor_min / anchor_max / pivot, for the preset picker. */
struct AnchorPresetInfo {
    const char* name;
    glm::vec2 amin, amax, pivot;
};
const std::vector<AnchorPresetInfo>& anchor_presets();

/**
 * @brief Applies an anchor preset the way Unity's picker does: the rect stays put; with
 *        `also_pivot` the pivot moves to the preset's too; with `also_position` the rect snaps
 *        to the anchors (zero position; a stretched axis fills its parent).
 */
void apply_anchor_preset(const Rect& parent, RectParams& p, const AnchorPresetInfo& preset, bool also_pivot, bool also_position);

/** @brief The preset whose anchors (and pivot) `p` has, or nullptr. */
const AnchorPresetInfo* matching_preset(const RectParams& p);

// =====================================================================================
// Snapping
// =====================================================================================

/** @brief One snap target along an axis: a coordinate (canvas px) and where the guide spans. */
struct SnapLine {
    int axis = 0;          ///< 0: a vertical line (x = value); 1: horizontal (y = value).
    float value = 0.0f;
    float from = 0.0f, to = 0.0f;   ///< The guide's extent along the other axis.
};

/** @brief The edges and centre of `r` along both axes, as snap targets. */
void add_rect_lines(std::vector<SnapLine>& out, const Rect& r);

/**
 * @brief Snaps a moving rect's edges/centre to the nearest target within `threshold` per
 *        axis (`edges` limits which features move: for a resize only the dragged edge).
 * @return The offset to add, and the lines it snapped to (for drawing guides).
 */
glm::vec2 snap_rect(const Rect& r, const std::vector<SnapLine>& lines, float threshold,
                           const bool features[2][3], std::vector<SnapLine>* hit = nullptr);

/** @brief Rounds `v` to the grid (`step` <= 0: unchanged). */
inline float snap_to_grid(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

// =====================================================================================
// The RectTransform YAML block
// =====================================================================================

/** @brief Reads a RectTransform block as uicoopa will (preset, then raw keys, then offsets). */
coopa::ui::RectTransform parse_rect_block(const Node& block);

/**
 * @brief Writes `p` (and rotation/scale/hittable/z_order) into a RectTransform block in the
 *        canonical form the editor edits: anchor_min/anchor_max/pivot/anchored_position/
 *        size_delta. anchor_preset and offset_* are removed, since uicoopa applies a preset
 *        first and offsets last -- leaving them would fight every later edit.
 */
void write_rect_params(Node& block, const RectParams& p);

glm::vec2 read_vec2(const Node& block, const std::string& key, glm::vec2 def);

coopa::ui::RectTransform parse_rect_block(const Node& block);

} // namespace toy::editor::ui

#endif // TOYEDITOR_UI_UI_CANVAS_MATH_H
