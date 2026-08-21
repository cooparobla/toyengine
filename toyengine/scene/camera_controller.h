/**
 * @file camera_controller.h
 * @brief Orbit and fly camera control, driving the owning SceneObject's Transform.
 *
 * No camera controller exists anywhere in the workspace -- blendy's camera
 * only ever moves via an orbit AnimationComponent orbiting a SEPARATE object
 * (coopa/scene/components/animation_component.h), never the camera itself.
 * This is greenfield.
 *
 * Per-frame input is pushed in by the caller (toy::core::Engine::tick(), via
 * Window::is_key_pressed()/cursor_position()) BEFORE SceneManager::update()
 * runs, not read directly from a Window here -- keeps this component testable
 * without a live GLFW window, matching toy::input::InputMap's own design.
 *
 * Orbit mode rotates the camera's current position and orientation by the
 * IDENTICAL rotation about the world Z (up) axis every frame -- a rigid
 * rotation applied equally to both preserves "looking at target" exactly,
 * regardless of which local axis the projection matrix treats as "forward".
 * This sidesteps needing to know or reverse-engineer that convention, unlike
 * a spherical-coordinates-plus-look-at-matrix implementation would.
 *
 * Fly mode's translation is similarly convention-free (it moves along the
 * CURRENT world matrix's own basis columns, whatever they are); only its
 * look-input handling assumes local Z is the forward axis (Blender's
 * convention, matching every camera authored elsewhere in this workspace),
 * since that can't be derived structurally the way translation can.
 */

#ifndef TOYENGINE_SCENE_CAMERA_CONTROLLER_H
#define TOYENGINE_SCENE_CAMERA_CONTROLLER_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/util/transform.h>

namespace toy {
namespace scene {

/**
 * @enum CameraControlMode
 * @brief Which control scheme CameraController applies each update().
 */
enum class CameraControlMode {
    Orbit, /**< Rotates around `target` at the current distance; auto-rotates with zero input. */
    Fly    /**< Free WASD + look movement, no target. */
};

/**
 * @class CameraController
 * @brief Drives the owning SceneObject's Transform in Orbit or Fly mode.
 *
 * Example YAML:
 * @code
 * - type: CameraController
 *   mode: orbit
 *   target: { x: 0.0, y: 0.0, z: 0.0 }
 *   auto_rotate_deg_per_sec: 20.0
 * @endcode
 */
class CameraController : public coopa::scene::Component {
public:
    std::string type_name() const override { return "CameraController"; }

    CameraControlMode mode = CameraControlMode::Orbit;

    // --- Orbit parameters ---
    glm::vec3 target                   = glm::vec3(0.0f);
    float     auto_rotate_deg_per_sec  = 20.0f;  /**< 0 disables auto-rotation. */
    float     orbit_yaw_speed_deg_per_sec = 90.0f;
    float     orbit_zoom_speed         = 5.0f;   /**< World units/sec at zoom_input == 1. */
    float     orbit_min_distance       = 0.5f;

    // --- Fly parameters ---
    float move_speed             = 5.0f;
    float look_speed_deg_per_sec = 90.0f;

    // --- Per-frame input, set by the caller before Scene::update() ---
    float     yaw_input  = 0.0f; /**< Orbit: -1..1 extra yaw beyond auto-rotate. */
    float     zoom_input = 0.0f; /**< Orbit: +1 = zoom in, -1 = zoom out. */
    glm::vec3 move_input = glm::vec3(0.0f); /**< Fly: x=strafe, y=forward, z=world-up. */
    glm::vec2 look_input = glm::vec2(0.0f); /**< Fly: x=yaw delta, y=pitch delta. */

    void update(float dt) override {
        if (!owner) return;
        auto* tc = owner->get_transform();
        if (!tc) return;
        auto& t = tc->transform();

        if (mode == CameraControlMode::Orbit) {
            update_orbit_(t, dt);
        } else {
            update_fly_(t, dt);
        }
    }

private:
    void update_orbit_(coopa::util::Transform& t, float dt) {
        float yaw_delta_deg = auto_rotate_deg_per_sec * dt + yaw_input * orbit_yaw_speed_deg_per_sec * dt;

        glm::vec3 offset = t.position() - target;
        if (zoom_input != 0.0f) {
            float dist = glm::max(orbit_min_distance, glm::length(offset) - zoom_input * orbit_zoom_speed * dt);
            if (glm::length(offset) > 0.0001f) offset = glm::normalize(offset) * dist;
        }

        if (yaw_delta_deg != 0.0f) {
            float rad = glm::radians(yaw_delta_deg);
            float c = std::cos(rad), s = std::sin(rad);
            offset = glm::vec3(offset.x * c - offset.y * s, offset.x * s + offset.y * c, offset.z);

            glm::vec3 rot = t.rotation_degrees();
            t.set_rotation(glm::vec3(rot.x, rot.y, rot.z + yaw_delta_deg));
        }

        t.set_position(target + offset);
    }

    void update_fly_(coopa::util::Transform& t, float dt) {
        glm::mat4 world = t.get_world_matrix();
        glm::vec3 right   = glm::normalize(glm::vec3(world[0]));
        glm::vec3 forward = -glm::normalize(glm::vec3(world[2])); // Blender: camera looks down local -Z

        glm::vec3 pos = t.position();
        pos += right * move_input.x * move_speed * dt;
        pos += forward * move_input.y * move_speed * dt;
        pos += glm::vec3(0.0f, 0.0f, 1.0f) * move_input.z * move_speed * dt; // world-up, avoids roll
        t.set_position(pos);

        if (look_input.x != 0.0f || look_input.y != 0.0f) {
            glm::vec3 rot = t.rotation_degrees();
            rot.z += look_input.x * look_speed_deg_per_sec * dt;
            rot.x = glm::clamp(rot.x + look_input.y * look_speed_deg_per_sec * dt, 1.0f, 179.0f);
            t.set_rotation(rot);
        }
    }
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CAMERA_CONTROLLER_H
