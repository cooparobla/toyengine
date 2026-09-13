/**
 * @file kinematic_mover.h
 * @brief Small scripted-motion component for kinematic Rigidbodies -- a moving platform, an
 *        orbiting carousel, or a spinning paddle -- driving the owning SceneObject's Transform
 *        directly. physxcoopa's PhysicsSystem::sync_transforms_in_() derives the kinematic
 *        body's linear/angular velocity from the resulting frame-to-frame Transform delta (see
 *        physxcoopa/system/physics_system.h), so this component needs no physics-specific code
 *        at all -- it only ever writes coopa::util::Transform, exactly like CameraController.
 */

#ifndef TOYENGINE_SCENE_KINEMATIC_MOVER_H
#define TOYENGINE_SCENE_KINEMATIC_MOVER_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

/** @brief Which scripted motion KinematicMover::update() applies each frame. */
enum class KinematicMoverMode {
    PingPong, /**< Oscillates along `axis` by `distance`, sinusoidally (smooth reversal --
                   no velocity discontinuity for PhysicsSystem's derived kinematic velocity
                   to react to, unlike a linear back-and-forth would produce at each end). */
    Orbit,    /**< Circles `orbit_center` at `orbit_radius` in the XY plane, at `speed`
                   degrees/sec, holding the seed Transform's Z height. */
    Spin,     /**< Rotates in place about `spin_axis` at `spin_speed` degrees/sec; position
                   never changes. */
};

/**
 * @class KinematicMover
 * @brief Scripts a kinematic Rigidbody's Transform -- pairs with `Rigidbody { is_kinematic:
 *        true }` on the same SceneObject in scene YAML.
 *
 * Example YAML:
 * @code
 * - type: KinematicMover
 *   mode: pingpong
 *   axis: { x: 1.0, y: 0.0, z: 0.0 }
 *   distance: 3.0
 *   speed: 0.5
 * @endcode
 */
class KinematicMover : public coopa::scene::Component {
public:
    std::string type_name() const override { return "KinematicMover"; }

    KinematicMoverMode mode = KinematicMoverMode::PingPong;

    // --- PingPong ---
    glm::vec3 axis{1.0f, 0.0f, 0.0f}; /**< Normalized internally; travel direction. */
    float distance = 2.0f;            /**< Peak-to-peak travel range, world units. */
    float speed = 1.0f;                /**< Oscillation frequency, Hz. */

    // --- Orbit ---
    glm::vec3 orbit_center{0.0f};
    float orbit_radius = 2.0f;
    // `speed` above doubles as the orbit's angular rate in degrees/sec.

    // --- Spin ---
    glm::vec3 spin_axis{0.0f, 0.0f, 1.0f};
    float spin_speed = 90.0f; /**< Degrees/sec. */

    /** @brief Captures the seed position/rotation every subsequent frame is computed relative to. */
    void start() override {
        if (!owner) return;
        auto* tc = owner->get_transform();
        if (!tc) return;
        origin_ = tc->transform().position();
        origin_rotation_ = tc->transform().rotation_quat();
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
     * @brief Advances the scripted motion by one frame.
     * @param dt Frame delta time in seconds.
     */
    void advance(float dt) {
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

private:
    glm::vec3 origin_{0.0f};
    glm::quat origin_rotation_{1.0f, 0.0f, 0.0f, 0.0f};
    float elapsed_ = 0.0f;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_KINEMATIC_MOVER_H
