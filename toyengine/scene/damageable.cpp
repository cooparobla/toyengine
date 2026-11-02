#include <toyengine/scene/damageable.h>

#include <physxcoopa/components/collider.h>
#include <physxcoopa/components/collision.h>
#include <physxcoopa/components/damage_dealer.h>
#include <physxcoopa/damage/impact.h>

#include <coopa/scene/scene_object.h>

namespace toy {
namespace scene {

void Damageable::start() {
    if (owner == nullptr) return;
    auto* collider = owner->get_component<coopa::physx::components::Collider>();
    if (collider == nullptr) return;
    impact_ = collider->on_collision_enter.connect_scoped(
        [this](coopa::physx::components::Collider&, const coopa::physx::components::Collision& hit) {
            if (destroyed_) return;
            if (glm::length(hit.impulse) < min_impulse) return; // a settling touch, not a blow

            coopa::physx::damage::DamageEvent blow = coopa::physx::damage::impact_blow(hit);

            // The OTHER collider's DamageDealer, if it has one, says what this blow actually
            // IS -- an axe head reports Slash, not the generic Impact impact_blow() assumed.
            coopa::physx::components::DamageDealer* dealer = nullptr;
            if (hit.collider != nullptr && hit.collider->owner != nullptr) {
                dealer = hit.collider->owner->get_component<coopa::physx::components::DamageDealer>();
            }
            if (dealer != nullptr) {
                if (dealer->one_shot && dealer->spent) return;
                blow.type = dealer->type;
                if (blow.type != coopa::physx::damage::DamageType::Impact) {
                    blow.amount = dealer->amount;
                } else {
                    blow.amount *= dealer->amount_from_impulse;
                }
                if (dealer->radius > 0.0f) blow.radius = dealer->radius;
                if (dealer->one_shot) dealer->spent = true;
            }
            apply_damage(blow);
        });
}

} // namespace scene
} // namespace toy
