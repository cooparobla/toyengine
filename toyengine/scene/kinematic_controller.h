/**
 * @file kinematic_controller.h
 * @brief Input-driven horizontal motion for a kinematic Rigidbody -- the player-controlled
 *        counterpart to KinematicMover's scripted motion.
 *
 * Like KinematicMover (and CameraController), this only ever writes coopa::util::Transform:
 * physxcoopa's PhysicsSystem::sync_transforms_in_() derives a kinematic body's linear velocity
 * from the resulting frame-to-frame Transform delta, so everything downstream -- contacts against
 * the body, cloth anchored to it -- follows with no physics-specific code here at all.
 *
 * Input is PUSHED in by Engine::drive_kinematic_controllers_(), never pulled: a component with no
 * reference to coopa::input::Input stays testable with no live GLFW window, and NO_INPUT=1 can
 * zero the whole thing for reproducible headless captures. Same contract CameraController follows.
 */

#ifndef TOYENGINE_SCENE_KINEMATIC_CONTROLLER_H
#define TOYENGINE_SCENE_KINEMATIC_CONTROLLER_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

/**
 * @class KinematicController
 * @brief Moves the owning object across the world XY plane from a pushed-in input vector, holding
 *        its authored height.
 *
 * Height is held rather than simulated because the intended pairing is a KINEMATIC Rigidbody
 * (`is_kinematic: true`, `use_gravity: false`): a kinematic body is authoritative over its own
 * pose by definition -- gravity and contacts never move it -- so "hover at the height I was placed
 * at" is the only behaviour consistent with that, and it is what makes the object a stable,
 * predictable anchor for anything attached to it.
 *
 * Example YAML:
 * @code
 * - type: KinematicController
 *   move_speed: 4.0
 *   smoothing: 10.0
 * @endcode
 */
class KinematicController : public coopa::scene::Component {
public:
    std::string type_name() const override { return "KinematicController"; }

    /** @brief Desired travel speed in m/s at full input deflection. */
    float move_speed = 4.0f;

    /** @brief Velocity smoothing rate (1/s); higher is snappier. Exponential, frame-rate
     *         independent (see update()), not a raw lerp factor -- a raw factor would make the
     *         object accelerate differently at 60 and 144 fps. 0 disables smoothing entirely.
     *
     *  Smoothing is not just feel here: PhysicsSystem derives this body's kinematic velocity from
     *  its frame-to-frame Transform delta, so an instant start/stop would hand the solver a
     *  velocity discontinuity, which anything resting on or pinned to the body feels as a jolt. */
    float smoothing = 10.0f;

    /** @brief Holds the seed Z from start(), so the object hovers instead of drifting vertically. */
    bool lock_height = true;

    /** @brief This frame's movement input, written by Engine before Scene::update(). `x` is world
     *         +X, `y` is world +Y; magnitude is clamped to 1 so diagonal travel is not faster. */
    glm::vec2 move_input{0.0f};

    /** @brief Captures the authored height every subsequent frame holds. */
    void start() override {
        if (!owner) return;
        auto* tc = owner->get_transform();
        if (!tc) return;
        height_ = tc->transform().position().z;
    }

    /**
     * @brief Intentionally empty -- the motion lives in advance(), driven by
     *        toy::scene::KinematicControlSystem at order 50.
     *
     * A component's update() runs at UpdatePhase::Behaviour (200), which is AFTER
     * UpdatePhase::Physics (100) has already read this object's Transform. Writing a kinematic
     * body's pose here would therefore always be one frame too late for the physics step that
     * consumes it -- see kinematic_control_system.h's file doc for the full trace.
     */
    void update(float) override {}

    /**
     * @brief Advances the owner along the world XY plane by one frame of smoothed `move_input`.
     * @param dt Frame delta time in seconds.
     */
    void advance(float dt) {
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

    /** @brief Current smoothed planar velocity in m/s -- for tests and gameplay readback. */
    const glm::vec2& velocity() const { return velocity_; }

private:
    glm::vec2 velocity_{0.0f};
    float height_ = 0.0f;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_KINEMATIC_CONTROLLER_H
