#include <toyengine/scene/destructible.h>

#include <algorithm>

#include <coopa/scene/scene_object.h>
#include <physxcoopa/components/collider.h>
#include <physxcoopa/components/collision.h>
#include <physxcoopa/components/rigidbody.h>
#include <coopa/scene/components/transform_component.h>

namespace toy {
namespace scene {

void Destructible::start() {
    if (owner == nullptr || impact_impulse <= 0.0f) return;
    auto* collider = owner->get_component<coopa::physx::components::Collider>();
    if (collider == nullptr) return;
    impact_ = collider->on_collision_enter.connect_scoped(
        [this](coopa::physx::components::Collider&, const coopa::physx::components::Collision& hit) {
            if (shattered_ || pending_) return;
            const float blow = glm::length(hit.impulse);
            if (blow < impact_impulse) return;

            // The speed the blow represents, for the chunks to carry on with: an impulse is
            // mass times the speed that was taken out of whatever hit this, so dividing by
            // that mass gives the speed back. A static or unknown hitter leaves it as-is.
            float mass = 1.0f;
            if (hit.object != nullptr) {
                if (auto* body = hit.object->get_component<coopa::physx::components::RigidbodyComponent>()) {
                    mass = std::max(0.01f, body->mass);
                }
            }
            const glm::vec3 point = hit.contact_count > 0 ? hit.contacts[0].position
                                                          : owner->get_transform()->transform().position();
            // `normal` points away from this surface, so the blow travels along -normal.
            shatter(point, -hit.normal * (blow / mass));
        });
}

} // namespace scene
} // namespace toy
