/**
 * @file ray.h
 * @brief Ray type, the ray-vs-AABB slab test used by broadphase traversal, and the exact
 *        analytic ray-vs-shape tests used by query/queries.h once broadphase has narrowed down
 *        candidates.
 */

#ifndef PHYSXCOOPA_GEOMETRY_RAY_H
#define PHYSXCOOPA_GEOMETRY_RAY_H

#include <physxcoopa/geometry/aabb.h>
#include <physxcoopa/geometry/sphere.h>
#include <physxcoopa/geometry/obb.h>
#include <physxcoopa/geometry/capsule.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @struct Ray
 * @brief A world-space ray with a maximum travel distance.
 */
struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; /**< Expected to be unit length. */
    float max_distance = std::numeric_limits<float>::max();

    /**
     * @brief Slab-test intersection against an AABB.
     *
     * Cribbed from gfxcoopa's GI-baking ray/box routine (gi_baker.h's intersect_box), stripped
     * of its GI-specific albedo/emissive payload -- only the entry distance is needed here.
     *
     * @param box Box to test against.
     * @param t   Output: entry distance along the ray, valid only when this returns true.
     * @return True if the ray hits the box within [0, max_distance].
     */
    bool intersect(const AABB& box, float& t) const {
        glm::vec3 inv_dir = 1.0f / (direction + glm::vec3(1e-9f));
        glm::vec3 t0 = (box.min - origin) * inv_dir;
        glm::vec3 t1 = (box.max - origin) * inv_dir;
        glm::vec3 tmin = glm::min(t0, t1);
        glm::vec3 tmax = glm::max(t0, t1);

        float t_near = std::max({tmin.x, tmin.y, tmin.z, 0.0f});
        float t_far = std::min({tmax.x, tmax.y, tmax.z, max_distance});

        if (t_far < t_near) return false;
        t = t_near;
        return true;
    }
};

/**
 * @brief Exact ray-vs-sphere intersection.
 *
 * @param ray    Query ray.
 * @param sphere Sphere to test.
 * @param t      Output: entry distance along the ray.
 * @param normal Output: surface normal at the hit point (points away from the sphere's center).
 * @return True if the ray hits the sphere within [0, ray.max_distance].
 */
inline bool ray_vs_sphere(const Ray& ray, const Sphere& sphere, float& t, glm::vec3& normal) {
    glm::vec3 oc = ray.origin - sphere.center;
    float b = glm::dot(oc, ray.direction);
    float c = glm::dot(oc, oc) - sphere.radius * sphere.radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0f) return false;

    float sqrt_disc = std::sqrt(discriminant);
    float t0 = -b - sqrt_disc;
    float t1 = -b + sqrt_disc;
    float hit = t0 >= 0.0f ? t0 : t1;
    if (hit < 0.0f || hit > ray.max_distance) return false;

    t = hit;
    normal = glm::normalize((ray.origin + ray.direction * t) - sphere.center);
    return true;
}

/**
 * @brief Exact ray-vs-OBB intersection, via the local-space slab test (Ray::intersect's slab
 *        test operates on an axis-aligned box; this transforms the ray into the box's local
 *        frame first, then converts the local-space normal back to world space).
 *
 * @param ray Query ray.
 * @param box Box to test.
 * @param t   Output: entry distance along the ray.
 * @param normal Output: world-space face normal at the hit point.
 * @return True if the ray hits the box within [0, ray.max_distance].
 */
inline bool ray_vs_obb(const Ray& ray, const OBB& box, float& t, glm::vec3& normal) {
    glm::quat inv_rot = glm::inverse(box.orientation);
    glm::vec3 local_origin = inv_rot * (ray.origin - box.center);
    glm::vec3 local_dir = inv_rot * ray.direction;

    glm::vec3 inv_dir = 1.0f / (local_dir + glm::vec3(1e-9f));
    glm::vec3 t0 = (-box.half_extents - local_origin) * inv_dir;
    glm::vec3 t1 = (box.half_extents - local_origin) * inv_dir;
    glm::vec3 tmin = glm::min(t0, t1);
    glm::vec3 tmax = glm::max(t0, t1);

    float t_near = std::max({tmin.x, tmin.y, tmin.z, 0.0f});
    float t_far = std::min({tmax.x, tmax.y, tmax.z, ray.max_distance});
    if (t_far < t_near) return false;

    int axis = 0;
    if (tmin.y > tmin.x) axis = 1;
    if (tmin.z > tmin[axis]) axis = 2;

    glm::vec3 local_normal(0.0f);
    local_normal[axis] = local_dir[axis] >= 0.0f ? -1.0f : 1.0f;

    t = t_near;
    normal = box.orientation * local_normal;
    return true;
}

/**
 * @brief Exact ray-vs-capsule intersection: nearest of the infinite-cylinder surface (clamped
 *        to the segment's span) and the two hemispherical end caps.
 *
 * @param ray     Query ray.
 * @param capsule Capsule to test.
 * @param t       Output: entry distance along the ray.
 * @param normal  Output: surface normal at the hit point.
 * @return True if the ray hits the capsule within [0, ray.max_distance].
 */
inline bool ray_vs_capsule(const Ray& ray, const Capsule& capsule, float& t, glm::vec3& normal) {
    glm::vec3 axis = capsule.b - capsule.a;
    float axis_len2 = glm::dot(axis, axis);
    bool found = false;
    float best_t = ray.max_distance;
    glm::vec3 best_normal(0.0f);

    if (axis_len2 > util::k_epsilon) {
        // Infinite-cylinder test, in the frame where the axis is the parameter direction:
        // decompose ray.direction and (ray.origin - a) into components parallel/perpendicular
        // to the capsule's axis, then solve the 2D (perpendicular-only) circle intersection.
        glm::vec3 axis_n = axis / std::sqrt(axis_len2);
        glm::vec3 oc = ray.origin - capsule.a;

        glm::vec3 d_perp = ray.direction - axis_n * glm::dot(ray.direction, axis_n);
        glm::vec3 oc_perp = oc - axis_n * glm::dot(oc, axis_n);

        float a = glm::dot(d_perp, d_perp);
        if (a > util::k_epsilon) {
            float b = glm::dot(d_perp, oc_perp);
            float c = glm::dot(oc_perp, oc_perp) - capsule.radius * capsule.radius;
            float discriminant = b * b - a * c;
            if (discriminant >= 0.0f) {
                float sqrt_disc = std::sqrt(discriminant);
                for (float candidate : {(-b - sqrt_disc) / a, (-b + sqrt_disc) / a}) {
                    if (candidate < 0.0f || candidate >= best_t) continue;
                    float along_axis = glm::dot((ray.origin + ray.direction * candidate) - capsule.a, axis_n);
                    if (along_axis < 0.0f || along_axis > std::sqrt(axis_len2)) continue;
                    glm::vec3 hit_point = ray.origin + ray.direction * candidate;
                    glm::vec3 closest_on_axis = capsule.a + axis_n * along_axis;
                    best_t = candidate;
                    best_normal = glm::normalize(hit_point - closest_on_axis);
                    found = true;
                    break; // the smaller root (tried first) is always the entry point when valid
                }
            }
        }
    }

    for (const glm::vec3& cap_center : {capsule.a, capsule.b}) {
        Sphere cap{cap_center, capsule.radius};
        float cap_t;
        glm::vec3 cap_normal;
        Ray capped_ray = ray;
        capped_ray.max_distance = best_t;
        if (ray_vs_sphere(capped_ray, cap, cap_t, cap_normal)) {
            best_t = cap_t;
            best_normal = cap_normal;
            found = true;
        }
    }

    if (!found) return false;
    t = best_t;
    normal = best_normal;
    return true;
}

/**
 * @brief Exact ray-vs-triangle intersection (Moller-Trumbore, single-sided: only hits the face
 *        `normal` from triangle_mesh.h's winding actually points toward).
 *
 * @param ray Query ray.
 * @param a,b,c Triangle vertices, wound so that `cross(b-a, c-a)` is the outward face normal.
 * @param t   Output: entry distance along the ray.
 * @return True if the ray hits the triangle within [0, ray.max_distance].
 */
inline bool ray_vs_triangle(const Ray& ray, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& t) {
    glm::vec3 edge1 = b - a;
    glm::vec3 edge2 = c - a;
    glm::vec3 pvec = glm::cross(ray.direction, edge2);
    float det = glm::dot(edge1, pvec);
    if (det > -util::k_epsilon && det < util::k_epsilon) return false;

    float inv_det = 1.0f / det;
    glm::vec3 tvec = ray.origin - a;
    float u = glm::dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;

    glm::vec3 qvec = glm::cross(tvec, edge1);
    float v = glm::dot(ray.direction, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;

    float candidate = glm::dot(edge2, qvec) * inv_det;
    if (candidate < 0.0f || candidate > ray.max_distance) return false;

    t = candidate;
    return true;
}

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_RAY_H
