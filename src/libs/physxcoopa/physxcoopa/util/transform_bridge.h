/**
 * @file transform_bridge.h
 * @brief World-space read/write helpers for coopa::util::Transform, kept out of libcoopa
 *        entirely -- Transform::parent() and get_world_matrix() are already public, so
 *        nothing here needs a libcoopa change.
 */

#ifndef PHYSXCOOPA_UTIL_TRANSFORM_BRIDGE_H
#define PHYSXCOOPA_UTIL_TRANSFORM_BRIDGE_H

#include <coopa/util/transform.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace coopa {
namespace physx {
namespace util {

/** @brief A decomposed world-space position + rotation (scale is not needed by physics). */
struct Trs {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

/**
 * @brief Decomposes a Transform's world matrix into position/rotation/scale.
 *
 * @param transform Transform to read (uses the lazy-recomputing get_world_matrix(), never
 *                   the non-recomputing world_matrix() -- see the plan's "Transform
 *                   resolution and read safety": physics runs before any resolve pass could
 *                   have completed this frame for objects it wasn't itself responsible for
 *                   dirtying).
 * @return World-space position/rotation/scale.
 */
inline Trs world_trs(const coopa::util::Transform& transform) {
    glm::mat4 m = transform.get_world_matrix();
    Trs out;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(m, out.scale, out.rotation, out.position, skew, perspective);
    return out;
}

/**
 * @brief Writes a world-space position/rotation into a Transform, converting to local space
 *        when parented.
 *
 * @param transform Transform to write (must already have its parent pointer set correctly).
 * @param position  Desired world-space position.
 * @param rotation  Desired world-space rotation.
 */
inline void set_world_trs(coopa::util::Transform& transform, const glm::vec3& position, const glm::quat& rotation) {
    const coopa::util::Transform* parent = transform.parent();
    if (!parent) {
        transform.set_position(position);
        transform.set_rotation_quat(rotation);
        return;
    }

    glm::mat4 parent_world = parent->get_world_matrix();
    glm::mat4 inv_parent = glm::inverse(parent_world);

    glm::mat4 world_target = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation);
    glm::mat4 local = inv_parent * world_target;

    glm::vec3 local_position;
    glm::quat local_rotation;
    glm::vec3 local_scale;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(local, local_scale, local_rotation, local_position, skew, perspective);

    transform.set_position(local_position);
    transform.set_rotation_quat(local_rotation);
}

} // namespace util
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_UTIL_TRANSFORM_BRIDGE_H
