/**
 * @file aabb.h
 * @brief Axis-aligned bounding box, used by broadphase proxies, mesh bounds, and queries.
 */

#ifndef PHYSXCOOPA_GEOMETRY_AABB_H
#define PHYSXCOOPA_GEOMETRY_AABB_H

#include <glm/glm.hpp>

#include <algorithm>
#include <limits>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @struct AABB
 * @brief Axis-aligned bounding box in world space.
 */
struct AABB {
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{std::numeric_limits<float>::lowest()};

    /** @brief Returns the smallest AABB containing both `a` and `b`. */
    static AABB merge(const AABB& a, const AABB& b) {
        AABB out;
        out.min = glm::min(a.min, b.min);
        out.max = glm::max(a.max, b.max);
        return out;
    }

    /** @brief True if this box and `other` overlap on all three axes (touching counts as overlap). */
    bool overlaps(const AABB& other) const {
        return min.x <= other.max.x && max.x >= other.min.x &&
               min.y <= other.max.y && max.y >= other.min.y &&
               min.z <= other.max.z && max.z >= other.min.z;
    }

    /** @brief True if `point` lies within this box on all three axes. */
    bool contains(const glm::vec3& point) const {
        return point.x >= min.x && point.x <= max.x &&
               point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    /** @brief True if this box fully contains `other`. */
    bool contains(const AABB& other) const {
        return contains(other.min) && contains(other.max);
    }

    /** @brief Returns a copy of this box grown by `margin` on every side. */
    AABB expand(float margin) const {
        AABB out;
        out.min = min - glm::vec3(margin);
        out.max = max + glm::vec3(margin);
        return out;
    }

    /** @brief Surface area of the box, used as the SAH cost metric for tree construction. */
    float surface_area() const {
        glm::vec3 e = extents();
        if (e.x < 0.0f || e.y < 0.0f || e.z < 0.0f) return 0.0f;
        return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
    }

    /** @brief Geometric center of the box. */
    glm::vec3 center() const { return 0.5f * (min + max); }

    /** @brief Full-extent (not half-extent) size of the box along each axis. */
    glm::vec3 extents() const { return max - min; }

    /**
     * @brief Transforms this AABB by a matrix, returning the (generally larger) AABB of the
     *        transformed box -- computed by transforming all 8 corners and re-fitting, which
     *        is exact under rotation (unlike center+half-extent transform shortcuts that only
     *        work for axis-aligned scale).
     *
     * @param m Transform matrix (may include rotation, scale, translation).
     * @return World-space AABB enclosing the transformed box.
     */
    AABB transform(const glm::mat4& m) const {
        AABB out;
        for (int i = 0; i < 8; ++i) {
            glm::vec3 corner(
                (i & 1) ? max.x : min.x,
                (i & 2) ? max.y : min.y,
                (i & 4) ? max.z : min.z);
            glm::vec3 world = glm::vec3(m * glm::vec4(corner, 1.0f));
            out.min = glm::min(out.min, world);
            out.max = glm::max(out.max, world);
        }
        return out;
    }
};

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_AABB_H
