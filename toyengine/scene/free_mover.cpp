#include <toyengine/scene/free_mover.h>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

void FreeMover::update(float delta_time) {
    if (!owner || delta_time <= 0.0f) return;
    auto* tc = owner->get_transform();
    if (!tc) return;

    glm::vec3 input = move_input;
    const float length = glm::length(input);
    if (length > 1.0f) input /= length; // clamp, don't normalize: partial stays partial

    const glm::vec3 target = input * move_speed;
    if (smoothing > 0.0f) {
        // 1 - exp(-k*dt) is the frame-rate-independent form of a lerp toward `target`: the
        // fraction of the gap closed over a given wall-clock interval is the same whatever
        // dt happens to be.
        velocity_ += (target - velocity_) * (1.0f - std::exp(-smoothing * delta_time));
    } else {
        velocity_ = target;
    }

    coopa::util::Transform& t = tc->transform();
    t.set_position(t.position() + velocity_ * delta_time);
}

} // namespace scene
} // namespace toy
