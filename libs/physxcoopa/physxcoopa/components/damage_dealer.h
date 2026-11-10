/**
 * @file damage_dealer.h
 * @brief Declares what a collider's hits ARE, so an axe head reports Slash and a bullet Pierce
 *        without gameplay code having to classify the blow itself.
 */

#ifndef PHYSXCOOPA_COMPONENTS_DAMAGE_DEALER_H
#define PHYSXCOOPA_COMPONENTS_DAMAGE_DEALER_H

#include <physxcoopa/damage/damage_types.h>

#include <coopa/scene/component.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class DamageDealer
 * @brief Put this on a Collider (a weapon head, a bullet, a falling weight) to say what kind of
 *        blow it deals, instead of every hit reading as a generic Impact.
 *
 * Consumed one layer up, by `toyengine::scene::Damageable`: on a collision between a Damageable
 * and a collider that carries a DamageDealer, the dealer's `type`/`edge_axis` override what
 * `damage::impact_blow()` would otherwise have guessed from the contact alone.
 */
class DamageDealer : public coopa::scene::Component {
public:
    std::string type_name() const override { return "DamageDealer"; }

    damage::DamageType type = damage::DamageType::Impact;

    /** @brief Amount for a non-impact-derived blow (abstract units, see DamageEvent::amount);
     *         ignored for Impact, which always reads its amount from the contact impulse. */
    float amount = 0.0f;

    /** @brief Scales the contact's impulse (N*s) into `DamageEvent::amount` for an Impact
     *         blow. 1 passes the impulse through unscaled. */
    float amount_from_impulse = 1.0f;

    /** @brief Slash only: this object's local axis along the blade's edge, carried into
     *         DamageEvent::edge at the contact point's orientation. */
    glm::vec3 edge_axis{0.0f, 1.0f, 0.0f};

    /** @brief 0 lets the receiving Damageable's own default choose a footprint. */
    float radius = 0.0f;

    /** @brief Deals its blow at most once, then stops (a thrown dart that embeds on the first
     *         hit, rather than every contact while it is still in flight). */
    bool one_shot = false;

    /** @brief Set once one_shot has fired; not for scenes to author. */
    bool spent = false;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_DAMAGE_DEALER_H
