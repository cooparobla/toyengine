#include <toyengine/scene/turntable.h>

#include <algorithm>
#include <cmath>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>

#include <glm/gtc/quaternion.hpp>

namespace toy {
namespace scene {
namespace {

/** @brief How fast the measured drag speed follows the current frame's, 1/s. */
constexpr float k_velocity_follow = 20.0f;

} // namespace

void Turntable::start() {
    if (!owner) return;
    auto* tc = owner->get_transform();
    if (!tc) return;
    const glm::vec3 seed = tc->transform().rotation_degrees();
    spin_deg_ = seed.z;
    tilt_deg_ = std::clamp(seed.x, min_tilt_deg, max_tilt_deg);
}

void Turntable::update(float delta_time) {
    if (!owner || delta_time <= 0.0f) return;
    auto* tc = owner->get_transform();
    if (!tc) return;

    const float dragged_spin = drag_input.x * sensitivity * (invert_x ? -1.0f : 1.0f);
    const float dragged_tilt = drag_input.y * sensitivity * (invert_y ? -1.0f : 1.0f);

    if (dragging) {
        // Dragging, the model tracks the cursor exactly; the velocity is only measured, so that
        // letting go hands the fling a speed the drag was actually travelling at. Measured as a
        // short average rather than from this one frame: a drag is a stream of pixel deltas, and
        // the last one before the button comes up is as likely to be a 1-pixel twitch as the
        // speed the hand was moving at.
        spin_deg_ += dragged_spin;
        tilt_deg_ = std::clamp(tilt_deg_ + dragged_tilt, min_tilt_deg, max_tilt_deg);
        const float measured = std::clamp(dragged_spin / delta_time, -max_spin_deg_per_sec, max_spin_deg_per_sec);
        spin_velocity_ += (measured - spin_velocity_) * (1.0f - std::exp(-k_velocity_follow * delta_time));
    } else {
        spin_deg_ += spin_velocity_ * delta_time;
        if (damping > 0.0f) {
            spin_velocity_ *= std::exp(-damping * delta_time);
            // Rest is rest: an exponential decay never reaches zero, and a model creeping round
            // at a thousandth of a degree a second keeps every temporal filter downstream (AO
            // accumulation, SSGI, antialiasing) churning for ever after the drag ended.
            if (std::abs(spin_velocity_) < 1e-3f) spin_velocity_ = 0.0f;
        }
    }
    spin_deg_ += idle_spin_deg_per_sec * delta_time;

    // Wrapped, so a long session's spin angle stays in a range float can still resolve degrees in.
    spin_deg_ = std::fmod(spin_deg_, 360.0f);

    const glm::vec3 spin_axis = glm::length(axis) > 1e-6f ? glm::normalize(axis) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 tilt_axis = glm::length(view_right) > 1e-6f ? glm::normalize(view_right) : glm::vec3(1.0f, 0.0f, 0.0f);

    // Tilt OUTSIDE spin: the spin is about the model's own pole, which the tilt then leans
    // toward the viewer. The other order would swing the pole round the screen as the model
    // spins, which is a trackball, not a globe.
    const glm::quat rotation = glm::angleAxis(glm::radians(tilt_deg_), tilt_axis) *
                               glm::angleAxis(glm::radians(spin_deg_), spin_axis);
    tc->transform().set_rotation_quat(rotation);
}

} // namespace scene
} // namespace toy
