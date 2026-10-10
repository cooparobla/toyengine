#include <toyengine/scene/kinematic_mover.h>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

void KinematicMover::start() {
    if (!owner) return;
    auto* tc = owner->get_transform();
    if (!tc) return;
    origin_ = tc->transform().position();
    origin_rotation_ = tc->transform().rotation_quat();
}

void KinematicMover::advance(float dt) {
    if (!owner) return;
    auto* tc = owner->get_transform();
    if (!tc) return;
    coopa::util::Transform& t = tc->transform();
    elapsed_ += dt;

    switch (mode) {
        case KinematicMoverMode::PingPong: {
            glm::vec3 dir = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(1.0f, 0.0f, 0.0f);
            float offset = std::sin(elapsed_ * speed * glm::two_pi<float>()) * (distance * 0.5f);
            t.set_position(origin_ + dir * offset);
            break;
        }
        case KinematicMoverMode::Orbit: {
            float angle_rad = glm::radians(speed * elapsed_);
            glm::vec3 pos = orbit_center + glm::vec3(std::cos(angle_rad), std::sin(angle_rad), 0.0f) * orbit_radius;
            pos.z = origin_.z;
            t.set_position(pos);
            break;
        }
        case KinematicMoverMode::Spin: {
            glm::vec3 ax = glm::length(spin_axis) > 1e-6f ? glm::normalize(spin_axis) : glm::vec3(0.0f, 0.0f, 1.0f);
            glm::quat delta = glm::angleAxis(glm::radians(spin_speed * elapsed_), ax);
            t.set_rotation_quat(delta * origin_rotation_);
            break;
        }
    }
}

} // namespace scene
} // namespace toy
