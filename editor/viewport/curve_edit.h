/**
 * @file curve_edit.h
 * @brief The pen tool's decisions, as pure functions over a `CurveData`.
 *
 * What is hit, where a click lands on the curve, and what each edit does -- none of it needs a
 * window, so all of it is unit-tested. The panel is left turning clicks into calls, the rule
 * `editor/mesh/` and `editor/node/` already follow.
 *
 * Everything here works in the curve's OWN space. The caller transforms the ray into it first
 * (the curve is local to its object), so nothing here needs to know about the scene graph.
 */

#ifndef TOYENGINE_EDITOR_VIEWPORT_CURVE_EDIT_H
#define TOYENGINE_EDITOR_VIEWPORT_CURVE_EDIT_H

#include <cstddef>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/curve/curve_data.h>

#include "editor/viewport/gizmo.h"

namespace toy {
namespace editor {
namespace curve_tool {

namespace cv = toy::curve;

/// @brief What the cursor is over.
enum class HitKind {
    None,
    Point,       ///< A control point's dot.
    InHandle,    ///< Its incoming handle.
    OutHandle,   ///< Its outgoing handle.
    Segment,     ///< The drawn line between two points -- where a click inserts.
};

/**
 * @struct CurveHit
 * @brief What `hit_test()` found.
 */
struct CurveHit {
    HitKind     kind = HitKind::None;
    std::size_t index = 0;   ///< The point, or for Segment the segment it belongs to.
    float       t = 0.0f;    ///< Segment only: where along it, 0..1.
    float       distance_px = 0.0f;

    explicit operator bool() const { return kind != HitKind::None; }
};

/**
 * @brief What is under `mouse`, in priority handle > point > segment.
 *
 * Handles win over points because they sit ON the curve near a point and would otherwise be
 * unreachable; a point wins over the segment that passes through it for the same reason.
 *
 * @param to_world  The curve's object-to-world matrix, for projecting its points.
 * @param pick_px   Pick radius in window pixels.
 * @param show_handles Handles are only hit-testable while they are drawn (a selected point).
 */
CurveHit hit_test(const cv::CurveData& c, const ViewProj& vp, const glm::mat4& to_world,
                  glm::vec2 mouse, float pick_px = 9.0f, bool show_handles = true);

/**
 * @brief Where a screen ray meets the plane through `origin` facing the camera.
 *
 * How a pen tool decides the depth of a click in a 3D view: a 2D cursor cannot name a point in
 * space, so one axis has to come from somewhere, and a camera-facing plane through the curve is
 * the choice that keeps a drawn point where the user saw it.
 */
glm::vec3 point_on_camera_plane(const ViewProj& vp, glm::vec2 mouse, const glm::vec3& origin);

/**
 * @brief Where a screen ray meets the world Z plane at `height`, or nullopt looking along it.
 *
 * The alternative depth rule, and the better one for laying a path over ground: points land on a
 * level plane rather than on a plane that tilts with the camera.
 */
std::optional<glm::vec3> point_on_ground(const ViewProj& vp, glm::vec2 mouse, float height);

/**
 * @brief Every anchor whose screen position falls inside the rectangle `a`-`b`.
 *
 * Corners in either order, so a band dragged up-left works like one dragged down-right.
 * Anchors behind the camera do not project and are never picked.
 */
std::vector<std::size_t> anchors_in_box(const cv::CurveData& c, const ViewProj& vp,
                                        const glm::mat4& to_world, glm::vec2 a, glm::vec2 b);

/// @brief Which end of the curve `index` is, for deciding whether a click extends or inserts.
bool is_endpoint(const cv::CurveData& c, std::size_t index);

/**
 * @brief Drags a handle to `target`, applying the point's mode.
 *
 * `Aligned` mirrors the opposite handle's DIRECTION while keeping its length, which is what
 * makes a dragged handle feel like it pivots the curve rather than reshaping both sides.
 * `Vector` and `Auto` become `Free` when dragged -- the handle the user just moved by hand
 * cannot also be derived from the neighbours.
 */
void drag_handle(cv::CurveData& c, std::size_t index, bool out_handle, const glm::vec3& target);

} // namespace curve_tool
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_VIEWPORT_CURVE_EDIT_H
