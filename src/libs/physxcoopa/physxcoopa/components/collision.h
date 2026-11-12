/**
 * @file collision.h
 * @brief Collision -- the payload passed to Collider::on_collision_enter/stay/exit, Unity's
 *        `Collision` parameter equivalent (as opposed to a trigger callback's bare Collider&,
 *        since Unity's OnTriggerEnter(Collider) never carries contact geometry either -- a
 *        trigger pair never reaches the solver, so there is no impulse/contact data to report).
 */

#ifndef PHYSXCOOPA_COMPONENTS_COLLISION_H
#define PHYSXCOOPA_COMPONENTS_COLLISION_H

#include <physxcoopa/collision/manifold.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace coopa {
namespace scene {
class SceneObject; // coopa/scene/scene_object.h -- forward declared, only ever used by pointer here.
} // namespace scene
} // namespace coopa

namespace coopa {
namespace physx {
namespace components {

class Collider; // collider.h -- forward declared, only ever used by pointer here.

/**
 * @struct Collision
 * @brief Contact-level detail for one collision event, from the perspective of the Collider
 *        whose signal is firing (`self`, implicit -- not stored here) against `collider`.
 *
 * `normal` points from `self` toward `collider` (i.e. away from self's own surface); `impulse`
 * is the accumulated normal impulse from this step's solve, in the same direction. Both are
 * zeroed (along with contacts/contact_count) on an Exit event, matching Unity's behavior of
 * reporting no contact points once a pair has already separated.
 */
struct Collision {
    Collider* collider = nullptr;             /**< The OTHER collider in this contact. */
    coopa::scene::SceneObject* object = nullptr; /**< collider's owning SceneObject, or nullptr. */

    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 relative_velocity{0.0f};         /**< self's linear velocity minus collider's. */
    glm::vec3 impulse{0.0f};                   /**< Accumulated normal impulse, along `normal`. */

    collision::ContactPoint contacts[4];
    uint8_t contact_count = 0;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_COLLISION_H
