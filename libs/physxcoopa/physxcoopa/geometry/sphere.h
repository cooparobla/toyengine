/**
 * @file sphere.h
 * @brief Sphere primitive.
 */

#ifndef PHYSXCOOPA_GEOMETRY_SPHERE_H
#define PHYSXCOOPA_GEOMETRY_SPHERE_H

#include <glm/glm.hpp>

namespace coopa {
namespace physx {
namespace geometry {

/**
 * @struct Sphere
 * @brief A sphere in world space.
 */
struct Sphere {
    glm::vec3 center{0.0f};
    float radius = 0.5f;
};

} // namespace geometry
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_GEOMETRY_SPHERE_H
