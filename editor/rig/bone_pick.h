/**
 * @file bone_pick.h
 * @brief Drawing and hit-testing one octahedral bone on screen, and picking the closest of
 *        a rig's bones under the cursor -- pure functions over head/tail positions and a
 *        ViewProj, no SceneObject, no coopa::rig::Rig, no window.
 *
 * `project_bone`/`in_triangle` are lifted unchanged from `editor/app/ui/viewport_chrome.cpp`
 * (which used them for the OLDER property-animation rig style, where a joint IS a SceneObject
 * and its bone glyph is drawn/picked by `viewport_chrome.cpp`'s own `draw_pose_bones_`/
 * `origin_glyph_pick_px_`). This file is the shared, unit-tested home for that math, used by
 * BOTH that older path and the new `coopa::rig::Rig` bone drawer/picker
 * (`editor/app/ui/viewport_chrome.cpp`'s `draw_rig_bones_`/`rig_bone_pick_px_`).
 */

#ifndef TOYENGINE_EDITOR_RIG_BONE_PICK_H
#define TOYENGINE_EDITOR_RIG_BONE_PICK_H

#include "editor/viewport/gizmo.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace toy {
namespace editor {
namespace rig_pick {

/**
 * @brief 2D point-in-triangle, for hit-testing a projected glyph face.
 *
 * False for a degenerate (edge-on) triangle: with no area, every point sits on all three
 * sides at once, which would otherwise claim the whole screen for a bone seen end-on.
 */
inline bool in_triangle(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    const float area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area2) < 1.0f) return false;  // under a square pixel of area
    const float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    const float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    const float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

/**
 * @brief The octahedral bone from `head` to `tail`, projected to the screen.
 *
 * `x_hint` rolls the body so it turns with its joint instead of spinning as the direction
 * crosses an axis; `z_hint` is the fallback when the bone runs along `x_hint`.
 *
 * @return False if any corner is behind the camera, or the bone has no length.
 */
inline bool project_bone(const ViewProj& vp, const glm::vec3& head, const glm::vec3& tail, const glm::vec3& x_hint,
                          const glm::vec3& z_hint, glm::vec2& head_px, glm::vec2& tail_px, glm::vec2 ring_px[4]) {
    const glm::vec3 along = tail - head;
    const float len = glm::length(along);
    if (len < 1e-5f) return false;
    const glm::vec3 dir = along / len;
    glm::vec3 u = x_hint - dir * glm::dot(x_hint, dir);
    if (glm::length(u) < 1e-4f) {
        u = z_hint - dir * glm::dot(z_hint, dir);
        if (glm::length(u) < 1e-4f) u = glm::cross(dir, std::abs(dir.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
    }
    u = glm::normalize(u);
    const glm::vec3 v = glm::cross(dir, u);
    const float r = 0.14f * len;
    const glm::vec3 mid = head + dir * r;
    const glm::vec3 ring[4] = {mid + u * r, mid + v * r, mid - u * r, mid - v * r};
    const auto h = vp.project(head), t = vp.project(tail);
    if (!h || !t) return false;
    for (int i = 0; i < 4; ++i) {
        const auto p = vp.project(ring[i]);
        if (!p) return false;
        ring_px[i] = *p;
    }
    head_px = *h;
    tail_px = *t;
    return true;
}

/** @brief One bone to draw/pick, in world space, with a roll hint for `project_bone`'s ring. */
struct BoneSegment {
    glm::vec3 head{0.0f};
    glm::vec3 tail{0.0f};
    glm::vec3 x_hint{1.0f, 0.0f, 0.0f};
    glm::vec3 z_hint{0.0f, 0.0f, 1.0f};
};

/**
 * @brief Screen-space distance from `px` to one projected bone; 0.0 if `px` is on its body
 *        (anywhere on either octahedral half, not just the outline -- a bone is a solid to
 *        click, same convention as a mesh face).
 *
 * @return `1e30f` if the bone does not project (behind the camera, or zero length).
 */
inline float bone_pick_distance(const ViewProj& vp, const BoneSegment& bone, glm::vec2 px) {
    glm::vec2 head, tip, q[4];
    if (!project_bone(vp, bone.head, bone.tail, bone.x_hint, bone.z_hint, head, tip, q)) return 1e30f;
    float best = 1e30f;
    for (int i = 0; i < 4; ++i) {
        const glm::vec2 b = q[i], c = q[(i + 1) % 4];
        if (in_triangle(px, head, b, c) || in_triangle(px, tip, b, c)) return 0.0f;
        best = std::min({best, point_segment_distance(px, head, b), point_segment_distance(px, b, tip),
                          point_segment_distance(px, b, c)});
    }
    return best;
}

/**
 * @brief The closest bone in `bones` to `px`, within `max_px` screen pixels.
 * @return Its index into `bones`, or -1 if none is within range (or on screen at all).
 */
inline int pick_closest_bone(const ViewProj& vp, const std::vector<BoneSegment>& bones, glm::vec2 px, float max_px = 10.0f) {
    int best_i = -1;
    float best_d = max_px;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const float d = bone_pick_distance(vp, bones[i], px);
        if (d <= best_d) {
            best_d = d;
            best_i = static_cast<int>(i);
        }
    }
    return best_i;
}

/** @brief Which endpoint a Rig Edit mode click landed on. */
enum class BoneEndpoint { Head, Tail };

/**
 * @brief The closest bone HEAD or TAIL (not the bone's body) to `px`, within `max_px` pixels --
 *        Rig Edit mode's drag-the-joint picking, where the body pick above (meant for Pose
 *        Mode's "grab this bone") is the wrong target: an endpoint drag must land on the exact
 *        point being moved, not anywhere on the octahedron.
 *
 * @return The bone index and which end, or `std::nullopt` if none is within range.
 */
inline std::optional<std::pair<int, BoneEndpoint>> pick_closest_endpoint(const ViewProj& vp, const std::vector<BoneSegment>& bones,
                                                                          glm::vec2 px, float max_px = 10.0f) {
    int best_i = -1;
    BoneEndpoint best_end = BoneEndpoint::Head;
    float best_d = max_px;
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const auto h = vp.project(bones[i].head), t = vp.project(bones[i].tail);
        if (h) {
            const float d = glm::distance(*h, px);
            if (d <= best_d) { best_d = d; best_i = static_cast<int>(i); best_end = BoneEndpoint::Head; }
        }
        if (t) {
            const float d = glm::distance(*t, px);
            if (d <= best_d) { best_d = d; best_i = static_cast<int>(i); best_end = BoneEndpoint::Tail; }
        }
    }
    if (best_i < 0) return std::nullopt;
    return std::make_pair(best_i, best_end);
}

/**
 * @brief What a Rig Edit click landed on: an endpoint handle, a bone's BODY, or nothing.
 *
 * Blender's three selectable parts of a bone, as one result -- `endpoint` set means a head/tail
 * handle, `bone >= 0` with no `endpoint` means the body (which selects both ends).
 */
struct BonePickResult {
    int bone = -1;
    std::optional<BoneEndpoint> endpoint;
    bool hit() const { return bone >= 0; }
};

/**
 * @brief Endpoints first at a TIGHTER radius, then the body -- so a click meant for a joint is
 *        never swallowed by the octahedron it sits on, while a click on the middle of a bone
 *        still grabs the whole bone.
 */
inline BonePickResult pick_bone_or_endpoint(const ViewProj& vp, const std::vector<BoneSegment>& bones, glm::vec2 px,
                                            float endpoint_px = 8.0f, float body_px = 10.0f) {
    BonePickResult out;
    if (const auto ep = pick_closest_endpoint(vp, bones, px, endpoint_px)) {
        out.bone = ep->first;
        out.endpoint = ep->second;
        return out;
    }
    out.bone = pick_closest_bone(vp, bones, px, body_px);
    return out;
}

} // namespace rig_pick
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_RIG_BONE_PICK_H
