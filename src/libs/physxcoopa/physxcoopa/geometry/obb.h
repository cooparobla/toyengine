/**
 * @file obb.h
 * @brief Oriented bounding box primitive and its closest-point query.
 */

#ifndef PHYSXCOOPA_GEOMETRY_OBB_H
#define PHYSXCOOPA_GEOMETRY_OBB_H

#include <physxcoopa/geometry/aabb.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @struct OBB
 * @brief A box in world space with an arbitrary orientation.
 */
struct OBB {
    glm::vec3 center{0.0f};
    glm::vec3 half_extents{0.5f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};

    /** @brief The box's local X/Y/Z axes in world space, unit length. */
    void axes(glm::vec3& x, glm::vec3& y, glm::vec3& z) const {
        glm::mat3 r = glm::mat3_cast(orientation);
        x = r[0];
        y = r[1];
        z = r[2];
    }

    /** @brief A world-space AABB that tightly encloses this OBB (exact, via 8-corner transform). */
    AABB bounds() const {
        AABB local;
        local.min = center - half_extents;
        local.max = center + half_extents;
        glm::mat4 m = glm::mat4_cast(orientation);
        m[3] = glm::vec4(center, 1.0f);
        AABB unrotated;
        unrotated.min = -half_extents;
        unrotated.max = half_extents;
        return unrotated.transform(m);
    }
};

/**
 * @brief Finds the closest point on (or in) an OBB's surface/volume to a world-space point.
 *
 * Projects the point into the box's local frame, clamps each axis to [-half_extent, half_extent],
 * then transforms back to world space. When `point` is inside the box, this returns `point`
 * itself (the clamp is a no-op on every axis) -- callers that need a *surface* point for an
 * interior query should treat a zero/near-zero (point - result) distance as "penetrating"
 * separately.
 *
 * @param point World-space query point.
 * @param box   Box to query against.
 * @return Closest point on/in the box, in world space.
 */
inline glm::vec3 closest_point_on_obb(const glm::vec3& point, const OBB& box) {
    glm::vec3 ax, ay, az;
    box.axes(ax, ay, az);
    glm::vec3 d = point - box.center;

    float px = std::clamp(glm::dot(d, ax), -box.half_extents.x, box.half_extents.x);
    float py = std::clamp(glm::dot(d, ay), -box.half_extents.y, box.half_extents.y);
    float pz = std::clamp(glm::dot(d, az), -box.half_extents.z, box.half_extents.z);

    return box.center + ax * px + ay * py + az * pz;
}

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_OBB_H
