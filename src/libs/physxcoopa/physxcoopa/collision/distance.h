/**
 * @file distance.h
 * @brief Exact closest-point queries between a segment (a sphere's or capsule's core -- a
 *        sphere is the zero-length case) and a box or a triangle. Shape casts
 *        (query/sweep.h) advance on these distances, so they must be exact rather than
 *        iterative: an over-estimated distance would let conservative advancement step past
 *        the true time of impact. Still analytic, no GJK (see shape.h's ShapeType doc).
 */

#ifndef PHYSXCOOPA_COLLISION_DISTANCE_H
#define PHYSXCOOPA_COLLISION_DISTANCE_H

#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/collision/mesh_contact.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @brief Closest points between segment [p0, p1] and an OBB, exactly.
 *
 * In the box's local frame the squared distance from the segment's point at parameter `s` to
 * the box is a sum, over the axes where that point lies outside the slab, of the squared
 * excess -- a piecewise quadratic in `s` whose pieces change only where the segment crosses a
 * slab plane (at most 6 breakpoints). Each piece's minimum is closed-form, so minimising over
 * every piece is exact. (closest_points_segment_obb() in segment.h is the iterative
 * alternative contact generation uses; it converges from above, which a cast cannot accept.)
 *
 * @param on_segment Output: closest point on the segment.
 * @param on_box     Output: closest point on/in the box (equal to `on_segment` when they touch).
 * @return Squared distance (0 when the segment touches or enters the box).
 */
inline float closest_points_segment_box(const glm::vec3& p0, const glm::vec3& p1, const geometry::OBB& box,
                                         glm::vec3& on_segment, glm::vec3& on_box) {
    glm::vec3 ax[3];
    box.axes(ax[0], ax[1], ax[2]);
    const glm::vec3 rel0 = p0 - box.center;
    const glm::vec3 rel1 = p1 - box.center;
    glm::vec3 o, d;
    for (int i = 0; i < 3; ++i) {
        o[i] = glm::dot(rel0, ax[i]);
        d[i] = glm::dot(rel1, ax[i]) - o[i];
    }
    const glm::vec3& h = box.half_extents;

    float breaks[8];
    int count = 0;
    breaks[count++] = 0.0f;
    breaks[count++] = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12f) continue;
        for (float plane : {-h[i], h[i]}) {
            float s = (plane - o[i]) / d[i];
            if (s > 0.0f && s < 1.0f) breaks[count++] = s;
        }
    }
    std::sort(breaks, breaks + count);

    auto sq_dist_at = [&](float s) {
        glm::vec3 p = o + d * s;
        glm::vec3 excess = p - glm::clamp(p, -h, h);
        return glm::dot(excess, excess);
    };

    float best_s = 0.0f;
    float best = sq_dist_at(0.0f);
    for (int k = 0; k + 1 < count; ++k) {
        float s0 = breaks[k], s1 = breaks[k + 1];
        if (s1 - s0 < 1e-9f) continue;
        // Inside one piece every axis is either clamped to a fixed face or free, so the
        // squared distance is sum((o_i - c_i + d_i * s)^2) over the clamped axes.
        glm::vec3 mid = o + d * (0.5f * (s0 + s1));
        float a = 0.0f, b = 0.0f;
        for (int i = 0; i < 3; ++i) {
            float c;
            if (mid[i] > h[i]) c = h[i];
            else if (mid[i] < -h[i]) c = -h[i];
            else continue;
            a += d[i] * d[i];
            b += d[i] * (o[i] - c);
        }
        float s = a > 1e-12f ? std::clamp(-b / a, s0, s1) : s0;
        float v = sq_dist_at(s);
        if (v < best) { best = v; best_s = s; }
    }
    float end_v = sq_dist_at(1.0f);
    if (end_v < best) { best = end_v; best_s = 1.0f; }

    glm::vec3 local = o + d * best_s;
    glm::vec3 clamped = glm::clamp(local, -h, h);
    on_segment = p0 + (p1 - p0) * best_s;
    on_box = box.center + ax[0] * clamped.x + ax[1] * clamped.y + ax[2] * clamped.z;
    return best;
}

/**
 * @brief Closest points between segment [p0, p1] and triangle (a, b, c), exactly (Ericson,
 *        "Real-Time Collision Detection", 5.1.10): zero if the segment pierces the triangle,
 *        otherwise the minimum over both endpoints against the triangle and the segment
 *        against each of the three edges. A zero-length segment reduces to
 *        closest_point_on_triangle().
 *
 * @return Squared distance.
 */
inline float closest_points_segment_triangle(const glm::vec3& p0, const glm::vec3& p1,
                                              const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                                              glm::vec3& on_segment, glm::vec3& on_triangle) {
    // Piercing test: the segment crosses the triangle's plane at a point inside it.
    glm::vec3 n = glm::cross(b - a, c - a);
    float d0 = glm::dot(p0 - a, n);
    float d1 = glm::dot(p1 - a, n);
    if ((d0 <= 0.0f && d1 >= 0.0f) || (d0 >= 0.0f && d1 <= 0.0f)) {
        if (std::abs(d0 - d1) > 1e-12f) {
            glm::vec3 q = p0 + (p1 - p0) * (d0 / (d0 - d1));
            glm::vec3 on = closest_point_on_triangle(q, a, b, c);
            glm::vec3 diff = q - on;
            if (glm::dot(diff, diff) < 1e-12f) {
                on_segment = q;
                on_triangle = on;
                return 0.0f;
            }
        }
    }

    float best = std::numeric_limits<float>::max();
    auto consider = [&](const glm::vec3& s, const glm::vec3& t) {
        glm::vec3 diff = s - t;
        float v = glm::dot(diff, diff);
        if (v < best) {
            best = v;
            on_segment = s;
            on_triangle = t;
        }
    };
    consider(p0, closest_point_on_triangle(p0, a, b, c));
    consider(p1, closest_point_on_triangle(p1, a, b, c));
    const glm::vec3* verts[3] = {&a, &b, &c};
    for (int e = 0; e < 3; ++e) {
        glm::vec3 on_s, on_e;
        geometry::closest_points_segment_segment(p0, p1, *verts[e], *verts[(e + 1) % 3], on_s, on_e);
        consider(on_s, on_e);
    }
    return best;
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_DISTANCE_H
