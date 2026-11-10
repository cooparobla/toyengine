/**
 * @file rect.h
 * @brief Pure anchor-based rect resolution, independent of Vulkan and coopa::scene.
 *
 * This is the mathematical core of uicoopa's RectTransform system, modeled directly
 * on Unity's RectTransform: a child rect is resolved against its parent's rect using
 * anchor_min/anchor_max (fractional reference points within the parent), a pivot
 * (fractional reference point within the child), an anchored_position (pivot offset
 * from the anchor reference point), and a size_delta (size relative to the anchor rect).
 *
 * Deliberately free of any dependency on coopa::scene::SceneObject, which transitively
 * includes Vulkan and caml headers — everything here is plain glm math and can be
 * exercised in a headless unit test.
 */

#ifndef UICOOPA_LAYOUT_RECT_H
#define UICOOPA_LAYOUT_RECT_H

#include <glm/glm.hpp>
#include <algorithm>

namespace coopa {
namespace ui {

/**
 * @struct Rect
 * @brief Axis-aligned rectangle in canvas pixel space.
 *
 * Origin is bottom-left, +Y up (Unity convention), independent of Vulkan's
 * NDC convention — the render layer is responsible for the Y flip.
 */
struct Rect {
    glm::vec2 min{0.0f}; /**< Bottom-left corner. */
    glm::vec2 max{0.0f}; /**< Top-right corner. */

    /** @brief Returns the width/height of the rect. */
    glm::vec2 size() const { return max - min; }

    /** @brief Returns the center point of the rect. */
    glm::vec2 center() const { return (min + max) * 0.5f; }
};

/**
 * @struct RectParams
 * @brief The Unity RectTransform parameter set, independent of any component or scene type.
 */
struct RectParams {
    glm::vec2 anchor_min{0.5f};        /**< Fraction of parent rect; (0,0)=bottom-left, (1,1)=top-right. */
    glm::vec2 anchor_max{0.5f};        /**< Fraction of parent rect; equal to anchor_min for a non-stretching rect. */
    glm::vec2 pivot{0.5f};             /**< Fraction of own rect that anchored_position addresses. */
    glm::vec2 anchored_position{0.0f}; /**< Pivot offset from the anchor reference point. */
    glm::vec2 size_delta{100.0f};      /**< Size relative to the anchor rect (== absolute size when anchors coincide). */
};

/**
 * @brief Resolves a child rect against its parent rect.
 *
 * Pure, allocation-free, and independent of any scene-graph type — safe to call
 * from a headless unit test. This is the single source of truth for anchor
 * resolution; every other rect-derived quantity in uicoopa (offsets, world
 * corners) is expressed in terms of it.
 *
 * @param parent The resolved parent rect, in canvas pixel space.
 * @param p The child's anchor/pivot/position/size parameters.
 * @return The resolved child rect, in the same space as parent.
 */
inline Rect resolve_rect(const Rect& parent, const RectParams& p) {
    Rect anchor_rect;
    anchor_rect.min = parent.min + parent.size() * p.anchor_min;
    anchor_rect.max = parent.min + parent.size() * p.anchor_max;

    glm::vec2 size = anchor_rect.size() + p.size_delta;
    glm::vec2 pivot_point = anchor_rect.min + anchor_rect.size() * p.pivot + p.anchored_position;

    Rect result;
    result.min = pivot_point - size * p.pivot;
    result.max = result.min + size;
    return result;
}

/**
 * @brief Returns the offset of the resolved rect's bottom-left corner from its anchor point.
 *
 * Matches Unity's RectTransform.offsetMin. Algebraically equal to
 * `anchored_position - pivot * size_delta` — provided for API symmetry with
 * resolve_rect and to make the relationship between offsets and the underlying
 * params explicit at call sites.
 *
 * @param parent The resolved parent rect.
 * @param p The child's rect parameters.
 * @return Offset in canvas pixels.
 */
inline glm::vec2 offset_min(const Rect& parent, const RectParams& p) {
    Rect r = resolve_rect(parent, p);
    glm::vec2 anchor_min_pos = parent.min + parent.size() * p.anchor_min;
    return r.min - anchor_min_pos;
}

/**
 * @brief Returns the offset of the resolved rect's top-right corner from its anchor point.
 *
 * Matches Unity's RectTransform.offsetMax. Algebraically equal to
 * `anchored_position + (1 - pivot) * size_delta`.
 *
 * @param parent The resolved parent rect.
 * @param p The child's rect parameters.
 * @return Offset in canvas pixels.
 */
inline glm::vec2 offset_max(const Rect& parent, const RectParams& p) {
    Rect r = resolve_rect(parent, p);
    glm::vec2 anchor_max_pos = parent.min + parent.size() * p.anchor_max;
    return r.max - anchor_max_pos;
}

/**
 * @brief Sets anchored_position and size_delta from the desired offset_min/offset_max.
 *
 * Matches Unity's offsetMin/offsetMax setters: anchor_min/anchor_max and pivot are
 * left unchanged, and anchored_position/size_delta are solved so that
 * offset_min(parent, p) == off_min and offset_max(parent, p) == off_max exactly.
 *
 * Offsets are anchor-relative by construction, so — unlike resolve_rect — the
 * solution does not depend on parent; the parameter exists for call-site symmetry
 * with offset_min/offset_max and is intentionally unused here.
 *
 * @param parent Unused; see above.
 * @param p The rect parameters to update in place.
 * @param off_min Desired offset_min.
 * @param off_max Desired offset_max.
 */
inline void set_offsets(const Rect& parent, RectParams& p, glm::vec2 off_min, glm::vec2 off_max) {
    (void)parent;
    glm::vec2 size_delta = off_max - off_min;
    p.anchored_position = off_min + p.pivot * size_delta;
    p.size_delta = size_delta;
}

/**
 * @brief Returns true if point lies within rect, inclusive of the boundary.
 */
inline bool contains(const Rect& r, const glm::vec2& point) {
    return point.x >= r.min.x && point.x <= r.max.x &&
           point.y >= r.min.y && point.y <= r.max.y;
}

/**
 * @brief Returns the overlapping region of two rects.
 *
 * If the rects do not overlap, returns a degenerate (zero-area) rect at the
 * point of closest approach rather than a rect with min > max.
 */
inline Rect intersect(const Rect& a, const Rect& b) {
    Rect result;
    result.min = glm::max(a.min, b.min);
    result.max = glm::min(a.max, b.max);
    result.max = glm::max(result.max, result.min);
    return result;
}

/**
 * @brief Slides `r` so it lies inside `bounds`, without resizing it.
 *
 * For floating elements that are positioned relative to something else and must not run off
 * the canvas -- a tooltip near the right edge, a popup near the bottom. Translation only:
 * keeping the size means a tooltip never reflows its text just because it drifted near an
 * edge, which would be far more distracting than the nudge.
 *
 * An `r` larger than `bounds` on an axis is pinned to that axis's minimum edge rather than
 * being pushed off the opposite one -- clamping the low edge last is what decides that.
 *
 * @param bounds The region to stay inside, typically CanvasComponent::root_rect().
 * @param r The rect to move.
 * @return `r` translated to fit.
 */
inline Rect clamp_inside(const Rect& bounds, const Rect& r) {
    const glm::vec2 size = r.size();
    glm::vec2 min = r.min;
    min = glm::min(min, bounds.max - size);
    min = glm::max(min, bounds.min);
    return Rect{ min, min + size };
}

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_LAYOUT_RECT_H
