/**
 * @file contact_event.h
 * @brief ContactEvent -- one enter/stay/exit notification, queued by PhysicsWorld during the
 *        substep loop and drained once per frame by the caller (PhysicsSystem, for a Scene-bound
 *        world; test.cpp directly, for headless use).
 *
 * Queued rather than fired immediately for the same reason on_substep defers structural
 * mutations (PhysicsWorld::create_body()/destroy_body()): a callback that reacts to a collision/trigger event might spawn or
 * destroy a body, which must not happen while the solver is mid-iteration over this step's
 * manifolds. This is a *different* queue from that one -- that one defers structural
 * *mutation*, this one defers *notification* -- both exist because either kind of callback
 * could otherwise corrupt an in-flight solve.
 */

#ifndef PHYSXCOOPA_COLLISION_CONTACT_EVENT_H
#define PHYSXCOOPA_COLLISION_CONTACT_EVENT_H

#include <physxcoopa/dynamics/body.h>

namespace coopa {
namespace physx {
namespace collision {

/**
 * @struct ContactEvent
 * @brief One (a, b) pair's enter/stay/exit transition this step, for either a solid contact or
 *        a trigger overlap (see `is_trigger`).
 */
struct ContactEvent {
    enum class Type { Enter, Stay, Exit };

    dynamics::BodyId a;
    dynamics::BodyId b;
    Type type = Type::Enter;
    bool is_trigger = false;
};

} // namespace collision
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COLLISION_CONTACT_EVENT_H
