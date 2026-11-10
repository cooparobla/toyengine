/**
 * @file segment.h
 * @brief Capsule (segment) contact generation against spheres, boxes and other capsules --
 *        all analytic, no GJK/EPA (see the plan's "Design rationale").
 */

#ifndef PHYSXCOOPA_COLLISION_SEGMENT_H
#define PHYSXCOOPA_COLLISION_SEGMENT_H

#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/sphere.h>
#include <physxcoopa/collision/manifold.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>

#include <cmath>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @brief Finds the closest points between a segment and an OBB via alternating projection:
 *        repeatedly clamp the current segment-side point onto the box, then clamp that
 *        box-side point back onto the segment. Both shapes are convex, so this converges
 *        quickly to the true closest pair (a handful of iterations is enough in practice) --
 *        a standard technique for closest-point-between-two-convex-shapes when a closed form
 *        would otherwise require casing on which Voronoi region of the box the segment falls
 *        into.
 *
 * @param a             Segment start.
 * @param b             Segment end.
 * @param box           Box to test against.
 * @param on_segment    Output: closest point on the segment.
 * @param on_box        Output: closest point on/in the box.
 */
inline void closest_points_segment_obb(const glm::vec3& a, const glm::vec3& b, const geometry::OBB& box,
                                        glm::vec3& on_segment, glm::vec3& on_box) {
    on_segment = 0.5f * (a + b);
    for (int i = 0; i < 8; ++i) {
        on_box = geometry::closest_point_on_obb(on_segment, box);
        on_segment = geometry::closest_point_on_segment(on_box, a, b);
    }
}

/**
 * @brief Capsule-vs-sphere contact.
 */
inline bool capsule_vs_sphere(const geometry::Capsule& cap, const geometry::Sphere& sphere,
                               glm::vec3& normal, float& penetration, glm::vec3& point) {
    glm::vec3 closest = geometry::closest_point_on_segment(sphere.center, cap.a, cap.b);
    glm::vec3 delta = sphere.center - closest;
    float dist2 = glm::dot(delta, delta);
    float radius_sum = cap.radius + sphere.radius;
    if (dist2 > radius_sum * radius_sum) return false;
    float dist = std::sqrt(dist2);
    normal = dist > util::k_epsilon ? delta / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    penetration = radius_sum - dist;
    point = closest + normal * cap.radius;
    return true;
}

/**
 * @brief Capsule-vs-capsule contact via the two segments' closest points.
 */
inline bool capsule_vs_capsule(const geometry::Capsule& a, const geometry::Capsule& b,
                                glm::vec3& normal, float& penetration, glm::vec3& point) {
    glm::vec3 c1, c2;
    geometry::closest_points_segment_segment(a.a, a.b, b.a, b.b, c1, c2);
    glm::vec3 delta = c2 - c1;
    float dist2 = glm::dot(delta, delta);
    float radius_sum = a.radius + b.radius;
    if (dist2 > radius_sum * radius_sum) return false;
    float dist = std::sqrt(dist2);
    normal = dist > util::k_epsilon ? delta / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    penetration = radius_sum - dist;
    point = c1 + normal * a.radius;
    return true;
}

/**
 * @brief Capsule-vs-box contact, promoting to a 2-point manifold when the segment lies flat
 *        against a box face (both endpoints similarly close to the same surface) rather than
 *        just poking a single point into it -- without this, a capsule lying on a flat floor
 *        would only ever get one contact point and could rock/roll around it.
 *
 * @param out Manifold filled with 1 or 2 points on success (`a`/`b`/material fields are left
 *            to the caller, matching collision::generate_contacts's convention).
 */
inline bool capsule_vs_box(const geometry::Capsule& cap, const geometry::OBB& box, ContactManifold& out) {
    glm::vec3 on_segment, on_box;
    closest_points_segment_obb(cap.a, cap.b, box, on_segment, on_box);
    glm::vec3 delta = on_segment - on_box;
    float dist = glm::length(delta);
    if (dist > cap.radius) return false;

    glm::vec3 primary_normal = dist > util::k_epsilon ? delta / dist : glm::vec3(0.0f, 0.0f, 1.0f);
    float primary_penetration = cap.radius - dist;

    out = ContactManifold{};
    out.normal = primary_normal;

    // Check whether both endpoints are similarly close to the box along the SAME normal --
    // that's the "segment lies flat against a face" case, worth two contact points instead
    // of just the single closest-approach point.
    glm::vec3 box_at_a = geometry::closest_point_on_obb(cap.a, box);
    glm::vec3 box_at_b = geometry::closest_point_on_obb(cap.b, box);
    float dist_a = glm::length(cap.a - box_at_a);
    float dist_b = glm::length(cap.b - box_at_b);

    constexpr float k_flat_tolerance = 0.02f;
    bool a_touches = dist_a <= cap.radius + k_flat_tolerance;
    bool b_touches = dist_b <= cap.radius + k_flat_tolerance;

    if (a_touches && b_touches) {
        glm::vec3 na = cap.a - box_at_a;
        glm::vec3 nb = cap.b - box_at_b;
        float na_len = glm::length(na);
        float nb_len = glm::length(nb);
        glm::vec3 normal_a = na_len > util::k_epsilon ? na / na_len : primary_normal;
        glm::vec3 normal_b = nb_len > util::k_epsilon ? nb / nb_len : primary_normal;
        // Only treat this as "flat" (not just two coincidentally-close points on different
        // faces, e.g. a capsule wedged into a corner) if both ends agree on the normal.
        if (glm::dot(normal_a, normal_b) > 0.9f) {
            out.normal = glm::normalize(normal_a + normal_b);
            out.add_point(box_at_a, std::max(cap.radius - dist_a, 0.0f), 0);
            out.add_point(box_at_b, std::max(cap.radius - dist_b, 0.0f), 1);
            return true;
        }
    }

    out.add_point(on_box, primary_penetration, 0);
    return true;
}

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_SEGMENT_H
