/**
 * @file capsule.h
 * @brief Capsule primitive (a swept sphere along a segment) and its segment-distance helpers.
 */

#ifndef PHYSXCOOPA_GEOMETRY_CAPSULE_H
#define PHYSXCOOPA_GEOMETRY_CAPSULE_H

#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>

#include <algorithm>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @struct Capsule
 * @brief A capsule: a line segment {a, b} swept by `radius`.
 */
struct Capsule {
    glm::vec3 a{0.0f, 0.0f, -0.5f};
    glm::vec3 b{0.0f, 0.0f, 0.5f};
    float radius = 0.5f;
};

/**
 * @brief Finds the closest point on segment [a, b] to `point`, and how far along the
 *        segment that point lies.
 *
 * @param point Query point.
 * @param a     Segment start.
 * @param b     Segment end.
 * @param t_out Output: parametric position in [0, 1] along the segment.
 * @return Closest point on the segment.
 */
inline glm::vec3 closest_point_on_segment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b, float& t_out) {
    glm::vec3 ab = b - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < util::k_epsilon) {
        t_out = 0.0f;
        return a;
    }
    float t = glm::dot(point - a, ab) / len2;
    t_out = std::clamp(t, 0.0f, 1.0f);
    return a + ab * t_out;
}

/** @brief Convenience overload discarding the parametric `t`. */
inline glm::vec3 closest_point_on_segment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b) {
    float t;
    return closest_point_on_segment(point, a, b, t);
}

/**
 * @brief Finds the closest points between two segments [p1,q1] and [p2,q2].
 *
 * Standard clamped-parametric approach (Ericson, "Real-Time Collision Detection", 5.1.9),
 * with the near-parallel-segments branch handled explicitly: when the segments' direction
 * vectors are (near-)parallel, the general 2x2 linear system used to solve for both
 * parameters simultaneously becomes singular (its determinant `denom` below tends to zero),
 * so that branch instead fixes s = 0 (the start of the first segment) and solves for t alone.
 *
 * @param p1   First segment's start.
 * @param q1   First segment's end.
 * @param p2   Second segment's start.
 * @param q2   Second segment's end.
 * @param c1   Output: closest point on the first segment.
 * @param c2   Output: closest point on the second segment.
 */
inline void closest_points_segment_segment(
    const glm::vec3& p1, const glm::vec3& q1,
    const glm::vec3& p2, const glm::vec3& q2,
    glm::vec3& c1, glm::vec3& c2) {

    glm::vec3 d1 = q1 - p1;
    glm::vec3 d2 = q2 - p2;
    glm::vec3 r = p1 - p2;

    float a = glm::dot(d1, d1); // squared length of segment 1
    float e = glm::dot(d2, d2); // squared length of segment 2
    float f = glm::dot(d2, r);

    float s, t;

    // Both segments degenerate to points.
    if (a < util::k_epsilon && e < util::k_epsilon) {
        c1 = p1;
        c2 = p2;
        return;
    }

    if (a < util::k_epsilon) {
        // First segment degenerates to a point.
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        float c = glm::dot(d1, r);
        if (e < util::k_epsilon) {
            // Second segment degenerates to a point.
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            float b = glm::dot(d1, d2);
            float denom = a * e - b * b; // >= 0, == 0 only when segments are parallel

            if (denom > util::k_epsilon) {
                s = std::clamp((b * f - c * e) / denom, 0.0f, 1.0f);
            } else {
                // Near-parallel: the general solve is ill-conditioned, so pick an
                // arbitrary (but valid) point on segment 1 and solve for t alone.
                s = 0.0f;
            }

            t = (b * s + f) / e;

            // If t fell outside [0,1], clamp it and re-solve for s against the clamped t --
            // otherwise the pair (s,t) doesn't correspond to the true closest points once t
            // is out of range.
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }

    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_CAPSULE_H
