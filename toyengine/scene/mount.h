/**
 * @file mount.h
 * @brief A rideable animal (a horse): steers its own CharacterController from the rider's
 *        input -- turning at a limited rate, always moving along its facing, through walk /
 *        trot / gallop -- and plays the matching gait clip at a speed matched to its ground
 *        speed. A Rider (rider.h) on the player mounts it.
 *
 * The horse is a CharacterController too (collision, gravity, slopes, steps, jumps), with
 * `player_input: false`: Engine::drive_character_controllers_() pushes the player's input into
 * the Mount instead, and only while it is ridden. Each frame, before the characters move
 * (KinematicControlSystem, order 50), steer() turns the facing toward the camera-relative input
 * direction at `turn_rate` (falling to `gallop_turn_rate` at full gallop), eases the speed toward
 * the gait's (`acceleration` / `braking`) and hands the controller "forward at that speed" -- a
 * horse never strafes. A hard turn (more than 100 degrees off) slows it to a walk.
 *
 * Gaits: input alone trots, `sprint` (Shift) gallops, `walk` (Ctrl) walks; no input stops. The
 * gait clips play at speed / `<gait>_speed`, so the speeds should be the clips' own travel per
 * second (tools/gen_horse.py prints them into the prefab).
 *
 * Sockets are paths from the horse (empty objects in its rig): the saddle the rider sits on,
 * the stirrups and reins the rider's feet and hands reach for, and the two ground-level mount
 * points beside the saddle where a rider gets on and off.
 *
 * Example YAML (objects/animals/horse):
 * @code
 * - type: CharacterController
 *   radius: 0.65
 *   player_input: false
 * - type: Mount
 *   walk_speed: 1.45
 *   trot_speed: 3.2
 *   gallop_speed: 8.9
 *   turn_rate: 150
 *   gallop_turn_rate: 70
 *   saddle: body/saddle
 *   stirrup_l: body/saddle/stirrup_l
 *   stirrup_r: body/saddle/stirrup_r
 *   rein_l: body/neck/rein_l
 *   rein_r: body/neck/rein_r
 *   mount_point_l: mount_point_l
 *   mount_point_r: mount_point_r
 *   camera_offset_z: 2.2
 *   camera_distance: 6.5
 * @endcode
 */

#ifndef TOYENGINE_SCENE_MOUNT_H
#define TOYENGINE_SCENE_MOUNT_H

#include <glm/glm.hpp>

#include <string>

#include <coopa/animation/animator.h>
#include <coopa/scene/component.h>

#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

class Rider;

/**
 * @class Mount
 * @brief A rideable CharacterController: gaits, rate-limited steering, gait animation and the
 *        rider's sockets (see the file doc).
 */
class Mount : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Mount"; }

    // --- Gaits (m/s; the gait clips' own ground speeds) ---
    float walk_speed = 1.45f;
    float trot_speed = 3.2f;
    float gallop_speed = 8.9f;
    float acceleration = 4.0f;       ///< m/s^2 speeding up.
    float braking = 6.0f;            ///< m/s^2 slowing down.
    float turn_rate = 150.0f;        ///< Degrees/s standing or walking...
    float gallop_turn_rate = 70.0f;  ///< ...falling to this at full gallop.
    float mount_max_speed = 0.8f;    ///< A rider can only get on below this speed.

    // --- Animation ---
    std::string idle_state = "idle";
    std::string walk_state = "walk";
    std::string trot_state = "trot";
    std::string gallop_state = "gallop";
    float crossfade = 0.25f;
    float min_anim_speed = 0.5f;     ///< Clamp on speed / gait speed (playback rate).
    float max_anim_speed = 1.6f;

    // --- Sockets (paths from this object) ---
    std::string saddle = "body/saddle";
    std::string stirrup_l = "body/saddle/stirrup_l";
    std::string stirrup_r = "body/saddle/stirrup_r";
    std::string rein_l = "body/neck/rein_l";
    std::string rein_r = "body/neck/rein_r";
    std::string mount_point_l = "mount_point_l";
    std::string mount_point_r = "mount_point_r";

    // --- The follow camera while ridden (the rider's CameraController) ---
    float camera_offset_z = 2.2f;
    float camera_distance = 6.5f;

    // --- Per-frame input, pushed by Engine::drive_character_controllers_() while ridden ---
    glm::vec2 move_input{0.0f};      ///< x = right, y = forward in the move basis (the camera's).
    float move_basis_yaw_deg = 0.0f;
    bool sprint = false;             ///< Held: gallop.
    bool walk = false;               ///< Held: walk.
    bool jump = false;               ///< A press: consumed by the next steer().

    /** @brief The rider on (or getting on / off) this mount, or null. */
    Rider* rider() const { return rider_; }
    bool is_ridden() const { return rider_ != nullptr; }

    /** @brief Free, on the ground and (nearly) standing still. */
    bool can_mount();

    /** @brief The horse's CharacterController (on this object), or null. */
    CharacterController* controller();

    /** @brief A socket object by path (saddle, stirrup_l, ...), or null. */
    coopa::scene::SceneObject* socket(const std::string& path);

    /** @brief Current forward speed (m/s) the steering asks for. */
    float speed() const { return speed_; }
    /** @brief The gait state last played ("" before the first update). */
    const std::string& gait() const { return current_; }

    /** @brief One frame of steering: turn, speed, and the controller's input. Called by
     *         KinematicControlSystem before the characters advance. */
    void steer(float dt);

    /** @brief The gait animation (after the controller has moved). */
    void update(float dt) override;

    /** @brief Claims / releases the mount (Rider only). */
    void set_rider(Rider* rider) { rider_ = rider; }

private:
    coopa::anim::Animator* animator_();

    Rider* rider_ = nullptr;
    CharacterController* controller_ = nullptr;
    coopa::anim::Animator* animator_cached_ = nullptr;
    float speed_ = 0.0f;
    float yaw_rate_ = 0.0f;   ///< Degrees/s turned last steer() (turning in place walks).
    std::string current_;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_MOUNT_H
