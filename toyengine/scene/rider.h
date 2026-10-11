/**
 * @file rider.h
 * @brief Lets the player's character get on a Mount (mount.h), ride it and get off: `interact`
 *        near one of the horse's mount points climbs into the saddle, and on a ridden horse it
 *        climbs down on a clear side.
 *
 * Getting on hands the character over: its CharacterController is suspended (the capsule is
 * disabled and the horse carries the body), its FootIK is suspended, the Animator crossfades to
 * `ride_state`, and the scene's CameraController follows the horse at the Mount's camera offset
 * and distance (restored on the way off). Getting off ray-casts the ground beside the chosen
 * mount point, checks the character's capsule fits there (the other side when not), and
 * teleports the controller there when the climb-down ends.
 *
 * Placement is an coopa::anim::IkDriver (IkSystem, order 320 -- after the Animator, before the
 * limb solvers): each frame the rider's root is put on the saddle (`seat_offset` in the saddle's
 * frame), so it rides the saddle's bob with no frame of lag; then the rig's own leg TwoBoneIKs
 * are pointed at the stirrups (knees pushed out round the barrel), its arm TwoBoneIKs at the
 * reins, and its spine ChainIK leans forward with the horse's speed. The rig must be IK-driven
 * like objects/characters/mannequin_ik (TwoBoneIKs ending at the feet and hands, a ChainIK
 * spine); a rig without them is still seated, posed by the ride clip alone. Climbing on and
 * off follows an arc between the mount point and the seat over `mount_time` / `dismount_time`,
 * the IK weights ramping with it.
 *
 * The Rider's IK `priority` defaults to 100, so IkSystem runs it after the horse's own drivers
 * (its FootIK raises and pitches the body, and so the saddle) whatever the scene order.
 *
 * `interact` is pushed by Engine::drive_character_controllers_() (the "interact" action, F).
 *
 * Example YAML (next to the player's CharacterController):
 * @code
 * - type: Rider
 *   mount_radius: 1.6
 *   mount_time: 0.8
 *   dismount_time: 0.6
 *   ride_state: ride
 *   seat_offset: {x: 0, y: -0.05, z: -0.86}
 *   knee_spread: 0.45
 *   lean: 0.15
 *   mount_on_start: horse   # optional: begin the scene in that Mount's saddle
 * @endcode
 */

#ifndef TOYENGINE_SCENE_RIDER_H
#define TOYENGINE_SCENE_RIDER_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

#include <coopa/animation/animator.h>
#include <coopa/animation/ik_components.h>

#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

class FootIK;
class Mount;

/**
 * @class Rider
 * @brief Mounts, rides and dismounts a Mount (see the file doc).
 */
class Rider : public coopa::anim::IkDriver {
public:
    std::string type_name() const override { return "Rider"; }

    enum class State { OnFoot, Mounting, Riding, Dismounting };

    Rider() { priority = 100; }   // after the mount's drivers (see the file doc)

    float mount_radius = 1.6f;      ///< Reach (m, horizontal) from the feet to a mount point.
    float mount_time = 0.8f;        ///< Seconds to climb on...
    float dismount_time = 0.6f;     ///< ...and off.
    std::string ride_state = "ride";
    float crossfade = 0.3f;
    /// The rider's root relative to the saddle socket (saddle frame): the mannequin's pelvis
    /// is 0.95 m above its root, so z ~ -0.9 sits it on the saddle.
    glm::vec3 seat_offset{0.0f, -0.05f, -0.86f};
    float knee_spread = 0.45f;      ///< Knee poles this far out from the hips (m), round the barrel.
    float lean = 0.15f;             ///< Spine target forward shift at full gallop (m).
    float leg_weight = 1.0f;
    float arm_weight = 1.0f;
    /// Name of a Mount's object to start the scene already seated on (no climb); empty: on foot.
    std::string mount_on_start;

    /// A press, pushed by the engine: get on (on foot, near a mount) or off (riding).
    bool interact = false;

    State state() const { return state_; }
    /** @brief On a mount, or getting on or off one. */
    bool is_mounted() const { return state_ != State::OnFoot; }
    /** @brief The mount ridden (or being got on / off), or null. */
    Mount* mount() const { return mount_; }

    /** @brief Starts getting on `m` from its nearer mount point. False when it is out of reach,
     *         taken, moving, or this rider is not on foot. */
    bool mount_onto(Mount* m);
    /** @brief The nearest free mount in reach, or null. */
    Mount* find_mount_in_reach();
    /** @brief Starts getting off on a clear side (the nearer one first). False when both sides
     *         are blocked or this rider is not riding. */
    bool dismount();

    /** @brief The interact press and the climb timers. */
    void update(float dt) override;

    /** @brief The seat and the limbs (see the file doc). */
    void ik_pre_solve(float dt) override;

private:
    struct Limb {
        coopa::anim::TwoBoneIK* ik = nullptr;
        std::string end;             ///< The end bone's name.
    };

    void bind_();
    bool mount_valid_();
    /** @brief The rider's root pose on the saddle this frame. */
    bool seat_pose_(glm::vec3& pos, glm::quat& rot);
    /** @brief A clear spot to stand on beside mount point `side` (0 = left, 1 = right). */
    bool dismount_spot_(int side, glm::vec3& feet);
    void set_root_(const glm::vec3& pos, const glm::quat& rot);
    void release_limbs_();
    void hand_over_(bool to_mount);
    void finish_dismount_();

    bool bound_ = false;
    bool start_checked_ = false;
    State state_ = State::OnFoot;
    Mount* mount_ = nullptr;
    CharacterController* controller_ = nullptr;
    FootIK* foot_ik_ = nullptr;
    coopa::anim::Animator* animator_ = nullptr;
    coopa::anim::ChainIK* spine_ = nullptr;
    Limb legs_[2];
    Limb arms_[2];
    float t_ = 0.0f;
    glm::vec3 from_pos_{0.0f};       ///< Climb start (on) / ground spot (off).
    glm::quat from_rot_{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 ground_feet_{0.0f};
    float ground_yaw_ = 0.0f;
    // The camera's follow settings before mounting.
    std::string cam_tracker_;
    glm::vec3 cam_offset_{0.0f};
    float cam_distance_ = -1.0f;
    bool cam_saved_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_RIDER_H
