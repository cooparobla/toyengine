#include <toyengine/scene/kinematic_controller.h>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

void KinematicController::start() {
    if (!owner) return;
    auto* tc = owner->get_transform();
    if (!tc) return;
    height_ = tc->transform().position().z;
}

void KinematicController::advance(float dt) {
    if (!owner || dt <= 0.0f) return;
    auto* tc = owner->get_transform();
    if (!tc) return;

    glm::vec2 input = move_input;
    const float len = glm::length(input);
    if (len > 1.0f) input /= len; // clamp, don't normalize: partial deflection stays partial

    const glm::vec2 target = input * move_speed;
    if (smoothing > 0.0f) {
        // 1 - exp(-k*dt) is the frame-rate-independent form of a lerp toward `target`: the
        // fraction covered over a given wall-clock interval is the same whatever dt is.
        velocity_ += (target - velocity_) * (1.0f - std::exp(-smoothing * dt));
    } else {
        velocity_ = target;
    }

    coopa::util::Transform& t = tc->transform();
    glm::vec3 p = t.position();
    p.x += velocity_.x * dt;
    p.y += velocity_.y * dt;
    if (lock_height) p.z = height_;
    t.set_position(p);
}

} // namespace scene
} // namespace toy
