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

    glm::vec2 to_canvas(glm::vec2 p) const {
        const float s = scale > 0.0f ? scale : 1.0f;
        return {(p.x - origin.x) / s, canvas_size.y - (p.y - origin.y) / s};
    }
    glm::vec2 to_editor(glm::vec2 c) const { return {origin.x + c.x * scale, origin.y + (canvas_size.y - c.y) * scale}; }
    /** @brief A canvas-space delta (Y up) for an editor-space delta (Y down). */
    glm::vec2 delta_to_canvas(glm::vec2 d) const {
        const float s = scale > 0.0f ? scale : 1.0f;
        return {d.x / s, -d.y / s};
    }
    /** @brief A canvas rect as an editor box (x, y = top-left). */
    std::array<float, 4> box_of(const Rect& r) const {
        const glm::vec2 tl = to_editor({r.min.x, r.max.y});
        return {tl.x, tl.y, r.size().x * scale, r.size().y * scale};
    }
};

/** @brief Point-in-convex-quad (corners in order, either winding). */
inline bool point_in_quad(const std::array<glm::vec2, 4>& q, glm::vec2 p) {
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 a = q[static_cast<size_t>(i)], b = q[static_cast<size_t>((i + 1) % 4)];
        const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (std::abs(cross) < 1e-6f) continue;
        const int s = cross > 0 ? 1 : -1;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
    }
    return true;
}

// =====================================================================================
// Rect edits -- each keeps whatever it is not changing VISUALLY fixed.
// =====================================================================================

/** @brief The resolved rect's offsets from its anchor rect: min = (left, bottom), max = (right, top). */
inline void offsets_of(const Rect& parent, const RectParams& p, glm::vec2& off_min, glm::vec2& off_max) {
    off_min = coopa::ui::offset_min(parent, p);
    off_max = coopa::ui::offset_max(parent, p);
}

/** @brief Re-expresses the rect `r` (canvas space) under the anchors/pivot already in `p`. */
inline void set_rect(const Rect& parent, RectParams& p, const Rect& r) {
    const glm::vec2 amin = parent.min + parent.size() * p.anchor_min;
    const glm::vec2 amax = parent.min + parent.size() * p.anchor_max;
    coopa::ui::set_offsets(parent, p, r.min - amin, r.max - amax);
}

/** @brief Moves the rect by `delta` canvas pixels. */
inline void move_by(RectParams& p, glm::vec2 delta) { p.anchored_position += delta; }

/**
 * @brief Resize from a handle. `hx`/`hy` say which edge moves: -1 the min (left/bottom)
 *        edge, +1 the max edge, 0 neither. The opposite edge stays put; `symmetric` moves
 *        both about the centre; `keep_aspect` holds the starting width:height ratio.
 * @param start The rect when the drag began (canvas space).
 * @param drag  Total cursor travel since then (canvas pixels, Y up).
 */
inline Rect resize_rect(const Rect& start, int hx, int hy, glm::vec2 drag, bool symmetric, bool keep_aspect,
                        float min_size = 1.0f) {
    Rect r = start;
    const glm::vec2 c = start.center();
    if (hx < 0) { r.min.x += drag.x; if (symmetric) r.max.x -= drag.x; }
    if (hx > 0) { r.max.x += drag.x; if (symmetric) r.min.x -= drag.x; }
    if (hy < 0) { r.min.y += drag.y; if (symmetric) r.max.y -= drag.y; }
    if (hy > 0) { r.max.y += drag.y; if (symmetric) r.min.y -= drag.y; }
    if (keep_aspect && hx != 0 && hy != 0 && start.size().y > 0.0f) {
        const float aspect = start.size().x / start.size().y;
        glm::vec2 sz = r.size();
        if (std::abs(sz.x / aspect) > std::abs(sz.y)) sz.y = sz.x / aspect;
        else sz.x = sz.y * aspect;
        if (symmetric) { r.min = c - sz * 0.5f; r.max = c + sz * 0.5f; }
        else {
            if (hx < 0) r.min.x = r.max.x - sz.x; else r.max.x = r.min.x + sz.x;
            if (hy < 0) r.min.y = r.max.y - sz.y; else r.max.y = r.min.y + sz.y;
        }
    }
    // Never invert: clamp the moving edge against the fixed one.
    if (r.max.x - r.min.x < min_size) { if (hx < 0) r.min.x = r.max.x - min_size; else r.max.x = r.min.x + min_size; }
    if (r.max.y - r.min.y < min_size) { if (hy < 0) r.min.y = r.max.y - min_size; else r.max.y = r.min.y + min_size; }
    return r;
}

/** @brief Sets anchors, keeping the rect where it is on screen. */
inline void set_anchors_keep_rect(const Rect& parent, RectParams& p, glm::vec2 amin, glm::vec2 amax) {
    const Rect r = coopa::ui::resolve_rect(parent, p);
    p.anchor_min = glm::clamp(amin, glm::vec2(0.0f), glm::vec2(1.0f));
    p.anchor_max = glm::clamp(glm::max(amax, p.anchor_min), glm::vec2(0.0f), glm::vec2(1.0f));
    set_rect(parent, p, r);
}

/** @brief Sets the pivot, keeping the rect where it is (anchored_position compensates). */
inline void set_pivot_keep_rect(const Rect& parent, RectParams& p, glm::vec2 pivot) {
    const Rect r = coopa::ui::resolve_rect(parent, p);
    p.pivot = pivot;
    set_rect(parent, p, r);
}

/** @brief Unity's anchor presets as anchor_min / anchor_max / pivot, for the preset picker. */
struct AnchorPresetInfo {
    const char* name;
    glm::vec2 amin, amax, pivot;
};
inline const std::vector<AnchorPresetInfo>& anchor_presets() {
    // Rows top / middle / bottom / stretch; columns left / center / right / stretch -- the
    // 4x4 grid Unity's picker shows.
    static const std::vector<AnchorPresetInfo> t = {
        {"TopLeft", {0, 1}, {0, 1}, {0, 1}},          {"TopCenter", {0.5f, 1}, {0.5f, 1}, {0.5f, 1}},
        {"TopRight", {1, 1}, {1, 1}, {1, 1}},         {"StretchTop", {0, 1}, {1, 1}, {0.5f, 1}},
        {"MiddleLeft", {0, 0.5f}, {0, 0.5f}, {0, 0.5f}}, {"MiddleCenter", {0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}},
        {"MiddleRight", {1, 0.5f}, {1, 0.5f}, {1, 0.5f}}, {"StretchHorizontal", {0, 0.5f}, {1, 0.5f}, {0.5f, 0.5f}},
        {"BottomLeft", {0, 0}, {0, 0}, {0, 0}},       {"BottomCenter", {0.5f, 0}, {0.5f, 0}, {0.5f, 0}},
        {"BottomRight", {1, 0}, {1, 0}, {1, 0}},      {"StretchBottom", {0, 0}, {1, 0}, {0.5f, 0}},
        {"StretchLeft", {0, 0}, {0, 1}, {0, 0.5f}},   {"StretchVertical", {0.5f, 0}, {0.5f, 1}, {0.5f, 0.5f}},
        {"StretchRight", {1, 0}, {1, 1}, {1, 0.5f}},  {"StretchAll", {0, 0}, {1, 1}, {0.5f, 0.5f}},
    };
    return t;
}

/**
 * @brief Applies an anchor preset the way Unity's picker does: the rect stays put; with
 *        `also_pivot` the pivot moves to the preset's too; with `also_position` the rect snaps
 *        to the anchors (zero position; a stretched axis fills its parent).
 */
inline void apply_anchor_preset(const Rect& parent, RectParams& p, const AnchorPresetInfo& preset, bool also_pivot, bool also_position) {
    if (also_pivot) set_pivot_keep_rect(parent, p, preset.pivot);
    set_anchors_keep_rect(parent, p, preset.amin, preset.amax);
    if (also_position) {
        for (int a = 0; a < 2; ++a) {
            if (preset.amin[a] != preset.amax[a]) {   // stretched: fill
                p.size_delta[a] = 0.0f;
                p.anchored_position[a] = 0.0f;
            } else {
                p.anchored_position[a] = 0.0f;
            }
        }
    }
}

/** @brief The preset whose anchors (and pivot) `p` has, or nullptr. */
inline const AnchorPresetInfo* matching_preset(const RectParams& p) {
    for (const auto& i : anchor_presets()) {
        if (glm::all(glm::lessThan(glm::abs(i.amin - p.anchor_min), glm::vec2(1e-4f))) &&
            glm::all(glm::lessThan(glm::abs(i.amax - p.anchor_max), glm::vec2(1e-4f)))) return &i;
    }
    return nullptr;
}

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
inline void add_rect_lines(std::vector<SnapLine>& out, const Rect& r) {
    for (int a = 0; a < 2; ++a) {
        const int b = 1 - a;
        for (float v : {r.min[a], r.center()[a], r.max[a]}) out.push_back({a, v, r.min[b], r.max[b]});
    }
}

/**
 * @brief Snaps a moving rect's edges/centre to the nearest target within `threshold` per
 *        axis (`edges` limits which features move: for a resize only the dragged edge).
 * @return The offset to add, and the lines it snapped to (for drawing guides).
 */
inline glm::vec2 snap_rect(const Rect& r, const std::vector<SnapLine>& lines, float threshold,
                           const bool features[2][3], std::vector<SnapLine>* hit = nullptr) {
    glm::vec2 best(0.0f);
    float best_d[2] = {threshold, threshold};
    const SnapLine* best_line[2] = {nullptr, nullptr};
    for (int a = 0; a < 2; ++a) {
        const float vals[3] = {r.min[a], r.center()[a], r.max[a]};
        for (const auto& l : lines) {
            if (l.axis != a) continue;
            for (int f = 0; f < 3; ++f) {
                if (!features[a][f]) continue;
                const float d = l.value - vals[f];
                if (std::abs(d) < best_d[a]) { best_d[a] = std::abs(d); best[a] = d; best_line[a] = &l; }
            }
        }
    }
    if (hit) for (int a = 0; a < 2; ++a) if (best_line[a]) hit->push_back(*best_line[a]);
    return best;
}

/** @brief Rounds `v` to the grid (`step` <= 0: unchanged). */
inline float snap_to_grid(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

// =====================================================================================
// The RectTransform YAML block
// =====================================================================================

/** @brief Reads a RectTransform block as uicoopa will (preset, then raw keys, then offsets). */
inline coopa::ui::RectTransform parse_rect_block(const Node& block);

/**
 * @brief Writes `p` (and rotation/scale/hittable/z_order) into a RectTransform block in the
 *        canonical form the editor edits: anchor_min/anchor_max/pivot/anchored_position/
 *        size_delta. anchor_preset and offset_* are removed, since uicoopa applies a preset
 *        first and offsets last -- leaving them would fight every later edit.
 */
inline void write_rect_params(Node& block, const RectParams& p) {
    auto v2 = [](glm::vec2 v) {
        Node n = Node::mapping();
        n["x"] = make_float(v.x);
        n["y"] = make_float(v.y);
        return n;
    };
    erase_key(block, "anchor_preset");
    erase_key(block, "offset_min");
    erase_key(block, "offset_max");
    block["anchor_min"] = v2(p.anchor_min);
    block["anchor_max"] = v2(p.anchor_max);
    block["pivot"] = v2(p.pivot);
    block["anchored_position"] = v2(p.anchored_position);
    block["size_delta"] = v2(p.size_delta);
}

inline glm::vec2 read_vec2(const Node& block, const std::string& key, glm::vec2 def) {
    if (!block.is_mapping() || !block.contains(key) || !block.at(key).is_mapping()) return def;
    return {get_float(block.at(key), "x", def.x), get_float(block.at(key), "y", def.y)};
}

inline coopa::ui::RectTransform parse_rect_block(const Node& block) {
    coopa::ui::RectTransform rt;
    if (!block.is_mapping()) return rt;
    if (block.contains("anchor_preset") && block.at("anchor_preset").is_string()) {
        const std::string name = block.at("anchor_preset").get_value<std::string>();
        for (const auto& i : anchor_presets()) {
            if (name == i.name) { rt.set_anchor_min(i.amin); rt.set_anchor_max(i.amax); rt.set_pivot(i.pivot); }
        }
    }
    rt.set_anchor_min(read_vec2(block, "anchor_min", rt.anchor_min()));
    rt.set_anchor_max(read_vec2(block, "anchor_max", rt.anchor_max()));
    rt.set_pivot(read_vec2(block, "pivot", rt.pivot()));
    rt.set_anchored_position(read_vec2(block, "anchored_position", rt.anchored_position()));
    rt.set_size_delta(read_vec2(block, "size_delta", rt.size_delta()));
    if (block.contains("offset_min") || block.contains("offset_max")) {
        const glm::vec2 omin = read_vec2(block, "offset_min", coopa::ui::offset_min(Rect{}, rt.params()));
        const glm::vec2 omax = read_vec2(block, "offset_max", coopa::ui::offset_max(Rect{}, rt.params()));
        coopa::ui::set_offsets(Rect{}, rt.params(), omin, omax);
    }
    if (block.contains("rotation")) rt.set_local_rotation_degrees(get_float(block, "rotation"));
    rt.set_local_scale(read_vec2(block, "scale", rt.local_scale()));
    if (block.contains("hittable")) rt.hittable = get_bool(block, "hittable", true);
    if (block.contains("z_order")) rt.z_order = get_int(block, "z_order");
    return rt;
}

} // namespace toy::editor::ui

#endif // TOYEDITOR_UI_UI_CANVAS_MATH_H
