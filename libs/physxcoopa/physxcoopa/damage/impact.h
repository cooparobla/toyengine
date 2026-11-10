/**
 * @file impact.h
 * @brief Turns a solved contact into an Impact DamageEvent -- the one place this library
 *        converts physics into damage.
 */

#ifndef PHYSXCOOPA_DAMAGE_IMPACT_H
#define PHYSXCOOPA_DAMAGE_IMPACT_H

#include <physxcoopa/components/collision.h>
#include <physxcoopa/damage/damage_types.h>

#include <glm/glm.hpp>

namespace coopa {
namespace physx {
namespace damage {

/**
 * @brief Turns a solved contact into an Impact blow.
 *
 * Reads the contact's accumulated NORMAL IMPULSE, not a speed: by the time the contact is
 * reported the solver has already stopped the thing that hit, so its relative velocity is only
 * the leftover -- the impulse is the solve's own output and is exactly mass times the speed
 * that was absorbed (the same reasoning `toy::scene::Destructible::impact_impulse`'s own doc
 * gives, one layer up).
 *
 * @param contact            The collision payload, from the Collider whose signal is firing.
 * @param contact_area_hint  0 lets the blow pick its own default footprint in resolve_damage();
 *                           a caller that knows the striking shape's contact patch (a hammer
 *                           head's face, say) can pass its radius here instead.
 */
inline DamageEvent impact_blow(const components::Collision& contact, float contact_area_hint = 0.0f) {
    DamageEvent blow;
    blow.type = DamageType::Impact;
    blow.amount = glm::length(contact.impulse);
    blow.point = contact.contact_count > 0 ? contact.contacts[0].position : glm::vec3(0.0f);
    blow.direction = blow.amount > 1e-6f ? contact.impulse / blow.amount : contact.normal;
    blow.radius = contact_area_hint;
    return blow;
}

} // namespace damage
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DAMAGE_IMPACT_H
