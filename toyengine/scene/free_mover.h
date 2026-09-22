/**
 * @file free_mover.h
 * @brief Input-driven free movement in all three world axes, for an object that is not a
 *        physics body -- a camera target, a focus marker, a probe you fly around a scene.
 *
 * The third member of this directory's small family of movers, and the one with the fewest
 * commitments. KinematicMover is scripted; KinematicController is input-driven but planar and
 * written for a kinematic Rigidbody (it holds its authored height, because a kinematic body is
 * authoritative over its own pose). This is input-driven, three-dimensional, and attached to
 * nothing: it moves a Transform and that is all it does.
 *
 * Because it drives nothing physical, it does its work in the ordinary `update()` at
 * UpdatePhase::Behaviour (200), unlike KinematicController -- which has to be hoisted to order
 * 50 by KinematicControlSystem so PhysicsSystem reads the pose that will actually be drawn (see
 * kinematic_control_system.h's file doc for that trace). There is no equivalent consumer here,
 * so there is no reason to run early.
 *
 * Input is PUSHED in by Engine::drive_free_movers_(), never pulled -- the same contract
 * CameraController and KinematicController follow. A component with no reference to
 * coopa::input::Input stays testable with no live GLFW window, and NO_INPUT=1 can zero the
 * whole thing for a reproducible headless capture.
 *
 * A FreeMover on an object with no renderer is an *invisible* marker, which is a genuinely
 * useful thing to be: toy::render's DOF autofocus resolves `focus_object` against the
 * transform origin when the target carries no bounds (see PixelRenderPipeline::
 * resolve_dof_focus_()), and CameraController's orbit `tracker` follows any named object. So
 * one of these plus a `Transform` is a drivable point in space that the camera can circle and
 * the lens can focus on, with nothing drawn for it -- which is exactly how
 * assets/scenes/terrain_test uses it.
 */

#ifndef TOYENGINE_SCENE_FREE_MOVER_H
#define TOYENGINE_SCENE_FREE_MOVER_H

#include <glm/glm.hpp>

#include <cmath>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

/**
 * @class FreeMover
 * @brief Moves the owning object along the world axes from a pushed-in 3D input vector.
 *
 * Motion is in WORLD axes, not the object's own basis: this is a point being positioned, not a
 * vehicle being flown, and a marker that rolled with its own orientation would be far harder to
 * place. (CameraController's Fly mode is the object-basis counterpart, for the case where
 * heading genuinely matters.)
 *
 * Example YAML:
 * @code
 * - type: FreeMover
 *   move_speed: 24.0
 *   smoothing: 12.0
 * @endcode
 */
class FreeMover : public coopa::scene::Component {
public:
    std::string type_name() const override { return "FreeMover"; }

    /** @brief Desired travel speed in world units per second at full input deflection. */
    float move_speed = 8.0f;

    /**
     * @brief Velocity smoothing rate (1/s); higher is snappier, 0 disables smoothing entirely.
     *
     * Exponential and frame-rate independent (`1 - exp(-k*dt)`, see update()), not a raw lerp
     * factor -- a raw factor would make the object accelerate differently at 60 and 144 fps.
     * Here it is purely feel, unlike KinematicController's identically-shaped smoothing, where
     * it also keeps the physics solver from being handed a velocity discontinuity.
     */
    float smoothing = 12.0f;

    /**
     * @brief This frame's movement input, written by Engine before Scene::update().
     *
     * `x` is world +X, `y` is world +Y, `z` is world +Z (up, in this engine's Z-up convention).
     * The magnitude is clamped to 1 rather than normalized, so travelling diagonally is not
     * faster than travelling along an axis, while a partial deflection stays partial.
     */
    glm::vec3 move_input{0.0f};

    /**
     * @brief Advances the owner along the world axes by one frame of smoothed `move_input`.
     * @param delta_time Frame delta time in seconds; a non-positive value is a no-op.
     */
    void update(float delta_time) override {
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

    /** @brief Current smoothed velocity in world units per second -- for tests and readback. */
    const glm::vec3& velocity() const { return velocity_; }

private:
    glm::vec3 velocity_{0.0f};
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_FREE_MOVER_H
