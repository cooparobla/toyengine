/**
 * @file camera_controller.h
 * @brief Mouse-orbit and fly camera control, driving the owning SceneObject's Transform.
 *
 * Orbit mode is a true spherical-coordinates rig: yaw/pitch/distance around a target point,
 * which may be a fixed world-space point or a named GameObject followed with exponential
 * smoothing (see `tracker`).
 *
 * `movement_smoothing` (0..1) adds drag to the camera's own response to mouse and scroll
 * input, independent of `follow_smoothing` (which only smooths a moving *target*'s
 * position). Yaw/pitch/distance are accumulated instantly into raw targets every frame; what
 * movement_smoothing changes is how quickly the applied pose chases those targets -- 0
 * chases them in the same frame, 1 chases them over roughly a couple of seconds without ever
 * fully stopping.
 *
 * Per-frame input is pushed in by the caller (toy::core::Engine::tick()) BEFORE
 * SceneManager::update() runs, rather than read from an Input or Window here -- that keeps
 * the component testable without a live GLFW window, matching InputMap's own design.
 *
 * Fly mode moves along the CURRENT world matrix's own basis columns (convention-free), and
 * its look handling assumes local Z is forward (Blender's convention, matching every camera
 * authored elsewhere in this workspace).
 */

#ifndef TOYENGINE_SCENE_CAMERA_CONTROLLER_H
#define TOYENGINE_SCENE_CAMERA_CONTROLLER_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>
#include <string>

#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
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
    Orbit, /**< Spherical orbit around a fixed point or tracked GameObject. */
    Fly    /**< Free WASD + look movement, no target. */
};

/**
 * @class CameraController
 * @brief Drives the owning SceneObject's Transform in Orbit or Fly mode.
 *
 * Orbit mode always looks directly at its target (a `tracker` GameObject
 * followed with `follow_smoothing`, or else the static `target` point, both
 * offset by `target_offset`). The mouse drives yaw/pitch continuously (no
 * button gate -- see Engine's cursor-capture wiring), pitch is clamped to
 * `[min_pitch_deg, max_pitch_deg]`, and the scroll wheel zooms `distance`
 * along the camera-to-target axis between `min_distance` and `max_distance`.
 * `movement_smoothing` (0..1) then adds drag to how quickly the camera's
 * actual pose chases that input -- see the file doc above.
 *
 * Example YAML:
 * @code
 * - type: CameraController
 *   mode: orbit
 *   tracker: player
 *   target_offset: { x: 0.0, y: 0.0, z: 1.0 }
 *   follow_smoothing: 8.0
 *   movement_smoothing: 0.3
 *   mouse_sensitivity: 0.15
 *   zoom_speed: 1.0
 *   min_distance: 1.0
 *   max_distance: 20.0
 *   min_pitch_deg: 0.0
 *   max_pitch_deg: 85.0
 * @endcode
 */
class CameraController : public coopa::scene::Component {
public:
    std::string type_name() const override { return "CameraController"; }

    CameraControlMode mode = CameraControlMode::Orbit;

    // --- Orbit target ---
    std::string tracker        = "";              /**< Name of a GameObject to follow; empty uses `target`. */
    glm::vec3   target         = glm::vec3(0.0f); /**< Static target point, used when `tracker` is empty or unresolved. */
    glm::vec3   target_offset  = glm::vec3(0.0f); /**< World-space offset added to the resolved target point. */
    float       follow_smoothing = 10.0f;         /**< Exponential follow rate (1/sec); <= 0 snaps instantly. */

    // --- Orbit geometry ---
    // distance/yaw_deg/pitch_deg default to sentinels meaning "derive from the
    // seed Transform in start()" (see start()'s own doc below). Set any of
    // them explicitly in YAML to skip that derivation for just that value.
    float distance  = -1.0f;         /**< Orbit radius; < 0 derives from the seed transform. */
    float yaw_deg   = kUnset;        /**< Orbit azimuth in degrees; kUnset derives from the seed transform. */
    float pitch_deg = kUnset;        /**< Orbit elevation in degrees; kUnset derives from the seed transform. */

    float min_pitch_deg = 0.0f;   /**< Lower elevation clamp. */
    float max_pitch_deg = 85.0f;  /**< Upper elevation clamp (kept off vertical to avoid a gimbal flip). */
    float min_distance  = 0.5f;   /**< Lower zoom clamp. */
    float max_distance  = 100.0f; /**< Upper zoom clamp. */

    // --- Orbit input tuning ---
    float mouse_sensitivity      = 0.15f; /**< Degrees of yaw/pitch per pixel of mouse motion. */
    bool  invert_x               = false; /**< Flips yaw direction. */
    bool  invert_y               = false; /**< Flips pitch direction. */
    float zoom_speed             = 1.0f;  /**< World units per unit of scroll input. */
    float auto_rotate_deg_per_sec = 0.0f; /**< Extra constant yaw added every frame; 0 disables it. */
    bool  capture_cursor         = true;  /**< Whether Engine should hide/unbound the cursor for this controller. */

    /**
     * Drag on the camera's own response to mouse/scroll input, clamped to
     * [0, 1]. 0 applies input the same frame it arrives (this component's
     * original, undamped behavior); 1 applies it with heavy lag -- roughly a
     * couple of seconds to catch up -- without ever fully freezing. Distinct
     * from follow_smoothing, which only smooths a moving tracker's position,
     * not the camera's own reaction to yaw/pitch/zoom input.
     */
    float movement_smoothing = 0.0f;

    // --- Fly parameters ---
    float move_speed             = 5.0f;
    float look_speed_deg_per_sec = 90.0f;

    // --- Per-frame input, set by the caller before Scene::update() ---
    glm::vec2 mouse_delta = glm::vec2(0.0f); /**< Orbit: raw mouse motion in pixels this frame. */
    float     scroll_input = 0.0f;           /**< Orbit: scroll wheel notches this frame (+ = zoom in). */
    glm::vec3 move_input = glm::vec3(0.0f);  /**< Fly: x=strafe, y=forward, z=world-up. */
    glm::vec2 look_input = glm::vec2(0.0f);  /**< Fly: x=yaw delta, y=pitch delta. */

    /**
     * @brief Seeds orbit geometry from the owner's initial Transform.
     *
     * Runs once when the scene finishes loading (see coopa::scene::Component::start()'s
     * own doc). Any of distance/yaw_deg/pitch_deg left at their sentinel default
     * is derived from the current position relative to the (possibly tracked)
     * target, so a scene authored by placing the camera by hand doesn't jump
     * on frame one.
     */
    void start() override {
        if (!owner) return;
        auto* tc = owner->get_transform();
        if (!tc) return;

        smoothed_target_ = resolve_target_();
        glm::vec3 offset = tc->transform().position() - smoothed_target_;
        float dist = glm::length(offset);

        if (distance < 0.0f) distance = glm::max(dist, min_distance);
        if (dist > 0.0001f) {
            if (yaw_deg == kUnset)   yaw_deg   = glm::degrees(std::atan2(offset.x, -offset.y));
            if (pitch_deg == kUnset) pitch_deg = glm::degrees(std::asin(glm::clamp(offset.z / dist, -1.0f, 1.0f)));
        } else {
            if (yaw_deg == kUnset)   yaw_deg   = 0.0f;
            if (pitch_deg == kUnset) pitch_deg = 0.0f;
        }
        pitch_deg = glm::clamp(pitch_deg, min_pitch_deg, max_pitch_deg);

        // The applied pose starts exactly at the raw seed -- no drag lag on frame one.
        smoothed_yaw_deg_   = yaw_deg;
        smoothed_pitch_deg_ = pitch_deg;
        smoothed_distance_  = distance;
    }

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

    /**
     * @brief Current smoothed camera-to-target orbit radius, in world units.
     *
     * Meaningful only in Orbit mode (0 in Fly mode, which has no target). Exposed
     * for DofPass's `dof_focus_mode: orbit_target` autofocus (see
     * PixelRenderPipeline::render()): the orbit rig already IS a camera-to-subject
     * distance, so autofocus reads this directly rather than re-deriving it.
     */
    float orbit_distance() const { return mode == CameraControlMode::Orbit ? smoothed_distance_ : 0.0f; }

private:
    /** @brief Sentinel meaning "not set in YAML, derive from the seed transform in start()". */
    static constexpr float kUnset = -10000.0f;

    // movement_smoothing==1 maps to this retention rather than to 1.0 exactly, so
    // "quite slow" never becomes "frozen forever" -- see update_orbit_()'s doc comment.
    static constexpr float kMaxMovementRetention  = 0.995f;
    // Reference frame rate the kMaxMovementRetention figure above was tuned against;
    // retention is raised to (dt * this) so the drag feels the same at any tick rate.
    static constexpr float kSmoothingReferenceFps = 60.0f;

    /**
     * @brief Resolves the current target point: the named `tracker` object's
     * world position if set and found, else the static `target` -- either
     * way plus `target_offset`.
     *
     * Resolved by name every call rather than caching a SceneObject*: this is
     * a linear DFS over a handful of objects (Scene::find_object()), which is
     * negligible next to the several full-scene traversals the render
     * pipeline already performs every frame, and it sidesteps a dangling
     * pointer if the tracked object is ever destroyed or the scene reloaded.
     */
    glm::vec3 resolve_target_() const {
        if (!tracker.empty() && scene) {
            if (auto* found = scene->find_object(tracker)) {
                if (auto* found_tc = found->get_transform()) {
                    return glm::vec3(found_tc->get_world_matrix()[3]) + target_offset;
                }
            } else if (!warned_missing_tracker_) {
                std::cerr << "[toyengine] CameraController: tracker \"" << tracker
                           << "\" not found; falling back to target.\n";
                warned_missing_tracker_ = true;
            }
        }
        return target + target_offset;
    }

    void update_orbit_(coopa::util::Transform& t, float dt) {
        glm::vec3 desired = resolve_target_();
        if (follow_smoothing <= 0.0f) {
            smoothed_target_ = desired;
        } else {
            float alpha = 1.0f - std::exp(-follow_smoothing * dt);
            smoothed_target_ += (desired - smoothed_target_) * alpha;
        }

        yaw_deg += mouse_delta.x * mouse_sensitivity * (invert_x ? -1.0f : 1.0f);
        yaw_deg += auto_rotate_deg_per_sec * dt;
        pitch_deg += mouse_delta.y * mouse_sensitivity * (invert_y ? -1.0f : 1.0f);
        pitch_deg = glm::clamp(pitch_deg, min_pitch_deg, max_pitch_deg);

        if (scroll_input != 0.0f) {
            distance = glm::clamp(distance - scroll_input * zoom_speed, min_distance, max_distance);
        }

        // Drag: yaw_deg/pitch_deg/distance above are the raw targets input just set;
        // smoothed_* is what's actually applied below, chasing those targets at a rate
        // set by movement_smoothing. At 0, retention is 0 and the branch below is
        // skipped entirely, so smoothed_* == raw every frame -- bit-for-bit the
        // original undamped behavior, not merely a fast approximation of it.
        float retention = glm::clamp(movement_smoothing, 0.0f, 1.0f) * kMaxMovementRetention;
        if (retention <= 0.0f) {
            smoothed_yaw_deg_   = yaw_deg;
            smoothed_pitch_deg_ = pitch_deg;
            smoothed_distance_  = distance;
        } else {
            // retention is "fraction of error remaining after one kSmoothingReferenceFps-
            // rate frame"; raising it to (dt * rate) generalizes that to the actual dt,
            // the same trick follow_smoothing's exp(-rate*dt) uses, just parameterized as
            // a 0..1 knob instead of a raw per-second rate.
            float alpha = 1.0f - std::pow(retention, dt * kSmoothingReferenceFps);
            smoothed_yaw_deg_   += (yaw_deg - smoothed_yaw_deg_) * alpha;
            smoothed_pitch_deg_ += (pitch_deg - smoothed_pitch_deg_) * alpha;
            smoothed_distance_  += (distance - smoothed_distance_) * alpha;
        }

        float e = glm::radians(smoothed_pitch_deg_);
        float phi = glm::radians(smoothed_yaw_deg_);
        glm::vec3 offset(
            smoothed_distance_ * std::cos(e) * std::sin(phi),
            -smoothed_distance_ * std::cos(e) * std::cos(phi),
            smoothed_distance_ * std::sin(e));

        t.set_position(smoothed_target_ + offset);
        t.set_rotation(glm::vec3(90.0f - smoothed_pitch_deg_, 0.0f, smoothed_yaw_deg_));
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

    glm::vec3 smoothed_target_ = glm::vec3(0.0f);
    float smoothed_yaw_deg_    = 0.0f; /**< Applied yaw; chases yaw_deg at a rate set by movement_smoothing. */
    float smoothed_pitch_deg_  = 0.0f; /**< Applied pitch; chases pitch_deg at a rate set by movement_smoothing. */
    float smoothed_distance_   = 0.0f; /**< Applied distance; chases distance at a rate set by movement_smoothing. */
    mutable bool warned_missing_tracker_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CAMERA_CONTROLLER_H
