/**
 * @file curve_edit.cpp
 * @brief Hit testing, depth rules and handle dragging for the pen tool.
 */

#include "editor/viewport/curve_edit.h"

#include <algorithm>
#include <cmath>

namespace toy {
namespace editor {
namespace curve_tool {
namespace {

/// @brief Projects a curve-space point to window pixels, or nullopt behind the camera.
std::optional<glm::vec2> to_px_(const ViewProj& vp, const glm::mat4& to_world, const glm::vec3& p) {
    return vp.project(glm::vec3(to_world * glm::vec4(p, 1.0f)));
}

/// @brief Considers `candidate` and keeps it when it is both within `pick_px` and the closest yet.
void consider_(CurveHit& best, HitKind kind, std::size_t index, float t, float d, float pick_px) {
    if (d > pick_px || (best && d >= best.distance_px)) return;
    best.kind = kind;
    best.index = index;
    best.t = t;
    best.distance_px = d;
}

} // namespace

CurveHit hit_test(const cv::CurveData& c, const ViewProj& vp, const glm::mat4& to_world,
                  glm::vec2 mouse, float pick_px, bool show_handles) {
    CurveHit best;

    // Handles first: they sit near their point and would be unreachable behind it otherwise.
    if (show_handles) {
        for (std::size_t i = 0; i < c.points.size(); ++i) {
            if (const auto p = to_px_(vp, to_world, c.points[i].in_handle)) {
                consider_(best, HitKind::InHandle, i, 0.0f, glm::distance(*p, mouse), pick_px);
            }
            if (const auto p = to_px_(vp, to_world, c.points[i].out_handle)) {
                consider_(best, HitKind::OutHandle, i, 0.0f, glm::distance(*p, mouse), pick_px);
            }
        }
    }

    // Points next, and they outrank a handle at the same distance: `consider_` keeps the first
    // strictly-closer hit, so the loop order decides ties, and a point under a handle is the
    // thing a user means to grab.
    for (std::size_t i = 0; i < c.points.size(); ++i) {
        if (const auto p = to_px_(vp, to_world, c.points[i].position)) {
            const float d = glm::distance(*p, mouse);
            if (d <= pick_px && (!best || d <= best.distance_px)) {
                best.kind = HitKind::Point;
                best.index = i;
                best.t = 0.0f;
                best.distance_px = d;
            }
        }
    }
    if (best) return best;

    // The drawn line last: walk each segment's samples and measure to the screen-space chords,
    // which is the same polyline the viewport drew, so the pick matches what is visible.
    const int res = std::max(1, c.resolution);
    for (std::size_t s = 0; s < c.segment_count(); ++s) {
        std::optional<glm::vec2> prev = to_px_(vp, to_world, cv::evaluate_segment(c, s, 0.0f));
        for (int i = 1; i <= res; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(res);
            const std::optional<glm::vec2> now = to_px_(vp, to_world, cv::evaluate_segment(c, s, t));
            if (prev && now) {
                const float d = point_segment_distance(mouse, *prev, *now);
                // The parameter at the nearer end of the sampled chord: close enough to split
                // where the user clicked, and insert_point() refines exactly rather than nudging.
                const float mid = (static_cast<float>(i) - 0.5f) / static_cast<float>(res);
                consider_(best, HitKind::Segment, s, mid, d, pick_px);
            }
            prev = now;
        }
    }
    return best;
}

glm::vec3 point_on_camera_plane(const ViewProj& vp, glm::vec2 mouse, const glm::vec3& origin) {
    glm::vec3 o, d;
    vp.ray(mouse, o, d);
    const glm::vec3 n = vp.camera_forward();
    const float denom = glm::dot(d, n);
    // Looking along the plane: there is no intersection, so keep the depth of `origin`.
    if (std::abs(denom) < 1e-6f) return origin;
    const float t = glm::dot(origin - o, n) / denom;
    if (t <= 0.0f) return origin;   // behind the camera
    return o + d * t;
}

std::optional<glm::vec3> point_on_ground(const ViewProj& vp, glm::vec2 mouse, float height) {
    glm::vec3 o, d;
    vp.ray(mouse, o, d);
    if (std::abs(d.z) < 1e-6f) return std::nullopt;   // the ray runs along the plane
    const float t = (height - o.z) / d.z;
    if (t <= 0.0f) return std::nullopt;               // the plane is behind the camera
    return o + d * t;
}

std::vector<std::size_t> anchors_in_box(const cv::CurveData& c, const ViewProj& vp,
                                        const glm::mat4& to_world, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
    const glm::vec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < c.points.size(); ++i) {
        const auto p = to_px_(vp, to_world, c.points[i].position);
        if (!p) continue;   // behind the camera
        if (p->x >= lo.x && p->x <= hi.x && p->y >= lo.y && p->y <= hi.y) out.push_back(i);
    }
    return out;
}

bool is_endpoint(const cv::CurveData& c, std::size_t index) {
    if (c.closed || c.points.empty()) return false;
    return index == 0 || index + 1 == c.points.size();
}

void drag_handle(cv::CurveData& c, std::size_t index, bool out_handle, const glm::vec3& target) {
    if (index >= c.points.size()) return;
    cv::CurvePoint& p = c.points[index];

    // A handle moved by hand cannot also be derived from the neighbours, so the mode gives way.
    if (p.mode == cv::HandleMode::Vector || p.mode == cv::HandleMode::Auto) {
        p.mode = cv::HandleMode::Free;
    }

    glm::vec3& dragged = out_handle ? p.out_handle : p.in_handle;
    glm::vec3& other   = out_handle ? p.in_handle : p.out_handle;
    dragged = target;

    if (p.mode != cv::HandleMode::Aligned) return;
    // Aligned: mirror the DIRECTION, keep the opposite handle's own length. Mirroring the length
    // too would make one short handle snap long the moment the other was touched.
    const glm::vec3 arm = dragged - p.position;
    const float arm_len = glm::length(arm);
    if (arm_len < 1e-6f) return;
    const float other_len = glm::length(other - p.position);
    other = p.position - (arm / arm_len) * other_len;
}

} // namespace curve_tool
} // namespace editor
} // namespace toy
