/**
 * @file character_controller.h
 * @brief A walking, jumping capsule character driven by pushed-in input -- the gameplay layer
 *        over physxcoopa's pure character motor (physxcoopa/character/character_motor.h).
 *
 * The motor does the geometry (depenetrate, collide-and-slide, step up, slope limit, ground
 * snap); this component owns everything with state: velocity and acceleration, gravity, jump
 * with coyote time and a jump buffer, riding moving platforms, pushing dynamic bodies, facing
 * the move direction, and the landed/jumped/collision signals.
 *
 * Like KinematicController, it is a KINEMATIC body that only ever writes its Transform: it
 * advances at order 50 in KinematicControlSystem, ahead of PhysicsSystem (100), so the physics
 * step sees this frame's pose, derives the body's velocity from the delta and pushes dynamic
 * bodies the capsule moves into. start() adds the kinematic Rigidbody and a CapsuleCollider
 * matching `radius`/`height` when the object has none, so a scene needs only this component.
 *
 * Input is PUSHED in (Engine::drive_character_controllers_()), never read here: `move_input` is
 * relative to `move_basis_yaw_deg` (the driver writes the main camera's yaw, so W walks away
 * from the camera), `jump` is a one-shot press the next advance() consumes, `sprint` is held.
 *
 * The Transform's origin is the character's FEET (the capsule centre is `height / 2` above it)
 * and the object faces local +Y. Put the component on a ROOT object: it writes the local
 * Transform as the world pose.
 *
 * Example YAML:
 * @code
 * - type: CharacterController
 *   radius: 0.3
 *   height: 1.8
 *   move_speed: 4.0
 *   sprint_multiplier: 1.8
 *   jump_height: 1.2
 * @endcode
 */

#ifndef TOYENGINE_SCENE_CHARACTER_CONTROLLER_H
#define TOYENGINE_SCENE_CHARACTER_CONTROLLER_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <coopa/animation/root_motion.h>

#include <physxcoopa/character/character_motor.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/save/saveable.h>

namespace toy {
namespace scene {

/**
 * @struct CharacterHit
 * @brief One surface the character moved into this frame (CharacterController::on_collision).
 */
struct CharacterHit {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};   ///< Toward the character.
    glm::vec3 move_direction{0.0f};       ///< Unit direction the capsule was moving in.
    coopa::physx::components::Collider* collider = nullptr;   ///< Null for a body with no bound Collider.
    coopa::scene::SceneObject* object = nullptr;
};

/**
 * @class CharacterController
 * @brief A capsule character: walks, sprints, jumps, climbs steps and walkable slopes, slides
 *        down steep ones, rides kinematic platforms and pushes dynamic bodies.
 */
class CharacterController : public coopa::scene::Component, public coopa::anim::IRootMotionReceiver,
                            public toy::save::ISaveable {
public:
    std::string type_name() const override { return "CharacterController"; }

    // --- Shape and walking rules (see coopa::physx::character::MotorSettings) ---
    float radius = 0.3f;
    float height = 1.8f;             ///< Total, caps included; the capsule centre is height/2 above the origin.
    float step_height = 0.35f;
    float slope_limit = 45.0f;       ///< Degrees.
    float skin = 0.02f;
    float snap_distance = 0.3f;

    // --- Movement ---
    float move_speed = 4.0f;         ///< m/s at full input.
    float sprint_multiplier = 1.8f;
    float acceleration = 30.0f;      ///< m/s^2 toward the target speed on the ground.
    float air_control = 0.3f;        ///< Fraction of `acceleration` available in the air.
    float gravity_scale = 1.0f;      ///< Times the physics world's gravity.
    float jump_height = 1.2f;        ///< Apex height of a jump, metres above the take-off.
    float coyote_time = 0.12f;       ///< A jump still works this long after walking off a ledge.
    float jump_buffer = 0.12f;       ///< A jump pressed this long before landing fires on landing.
    bool face_movement = true;       ///< Turn to face the horizontal move direction.
    float turn_speed = 720.0f;       ///< Degrees per second when facing the move direction.

    // --- Pushing ---
    bool push_dynamic_bodies = true;
    float push_strength = 80.0f;     ///< The character's pushing mass (kg): a body of mass m reaches
                                     ///< push_strength / (push_strength + m) of the walking speed.

    /** @brief Move by the displacement fed through add_root_motion() (an Animator's root motion,
     *         see consume_root_motion()) instead of `move_input` -- collisions, gravity and jumps
     *         still apply, and `move_input` still turns the character (face_movement).
     *
     *         Grounded only: in the air the character keeps the velocity it left the ground with,
     *         steered by `move_input` (`air_control`) and slid by steep slopes, as without root
     *         motion -- an airborne clip (a jump/fall pose) carries no travel, and following it
     *         would leave a character resting on something it can't stand on (wedged against a
     *         steep slope's foot) with no way to move, falling forever. */
    bool use_root_motion = false;

    // --- Per-frame input, pushed by Engine::drive_character_controllers_() ---
    glm::vec2 move_input{0.0f};      ///< x = right, y = forward, in the move basis; length clamped to 1.
    float move_basis_yaw_deg = 0.0f; ///< Yaw of the move basis (the camera's); 0 = world axes (x -> +X, y -> +Y).
    bool jump = false;               ///< A press: consumed (reset) by the next advance().
    bool sprint = false;             ///< Held.

    // --- Signals ---
    coopa::event::Signal<float> on_landed;                  ///< Downward speed at touch-down (m/s).
    coopa::event::Signal<> on_jumped;
    coopa::event::Signal<const CharacterHit&> on_collision; ///< Every surface moved into, each frame.

    // --- State ---
    bool is_grounded() const { return grounded_; }
    /** @brief Velocity of the character's own motion (world, m/s), platform carry excluded. */
    glm::vec3 velocity() const { return glm::vec3(h_velocity_, v_velocity_); }
    const glm::vec3& ground_normal() const { return ground_normal_; }
    float yaw_deg() const { return yaw_deg_; }
    /** @brief The last move's motor result -- its collisions, step/snap flags, ground body. */
    const coopa::physx::character::MoveResult& last_move() const { return last_move_; }

    /**
     * @brief Hands the character over to something else (a Ragdoll) or takes it back: while
     *        suspended advance() does nothing and the capsule collider is disabled, so the body
     *        neither moves nor blocks. Resuming re-enables the capsule; teleport() it first if the
     *        character should stand somewhere new.
     */
    void set_suspended(bool suspended);
    bool is_suspended() const { return suspended_; }

    /** @brief Teleports the feet to `p` (world), clearing velocity. */
    void teleport(const glm::vec3& p);

    /** @brief Teleports the feet to `p` and turns to face `yaw_deg` (0 faces +Y). */
    void teleport(const glm::vec3& p, float yaw_deg);

    // --- Saves (toyengine/save/): with a SaveId on the object, its pose goes in the save ---
    std::string save_key() const override { return "character"; }
    void save(toy::save::SaveNode& out) override;
    void load(const toy::save::SaveNode& in) override;

    /** @brief Sets the facing yaw (degrees; 0 faces +Y). Used by a FirstPerson CameraController:
     *         while this is fed each frame, face_movement does not turn the character. */
    void set_facing_yaw(float yaw_deg);

    /** @brief Adds an animation's root-motion step: `local_delta` in the character's own frame
     *         (+Y forward), `yaw_delta_deg` turning it. Consumed by the next advance() when
     *         `use_root_motion` is on; ignored otherwise. */
    void add_root_motion(const glm::vec3& local_delta, float yaw_delta_deg);

    /** @brief An Animator's root motion (Animator::apply_root_motion on this object): taken
     *         only while `use_root_motion` is on, else left for the Animator to apply itself. */
    bool consume_root_motion(const glm::vec3& local_delta, float yaw_delta_deg) override;

    /** @brief The motor settings this controller's fields describe. */
    coopa::physx::character::MotorSettings motor_settings() const;

    /** @brief Adds the kinematic Rigidbody and CapsuleCollider when missing; seeds the facing. */
    void start() override;

    /** @brief Intentionally empty -- the motion lives in advance() (order 50, ahead of physics),
     *         for the reason kinematic_control_system.h's file doc gives. */
    void update(float) override {}

    /**
     * @brief One frame: velocity from input, gravity and jump, then the motor move, then the
     *        Transform write.
     * @param dt Frame delta time in seconds.
     * @param physics The scene's PhysicsSystem (nullptr: the character doesn't move).
     */
    void advance(float dt, coopa::physx::system::PhysicsSystem* physics);

private:
    /** @brief Gives the body a velocity change along the push direction, inelastic-collision
     *         style (see push_strength), never slowing it down. */
    void push_(coopa::physx::PhysicsWorld& world, const coopa::physx::character::MotorHit& hit, float speed);

    /** @brief World matrix of the object owning `body` (resolved through the physics system
     *         every time, so a destroyed ground object is never dereferenced). */
    static bool ground_matrix_(coopa::physx::system::PhysicsSystem* physics, coopa::physx::dynamics::BodyId body,
                               glm::mat4& out);

    /** @brief The kinematic Rigidbody + CapsuleCollider this needs, added when missing. */
    void ensure_body_();

    coopa::physx::components::RigidbodyComponent* rigidbody_ = nullptr;
    bool suspended_ = false;
    glm::vec2 h_velocity_{0.0f};
    float v_velocity_ = 0.0f;
    bool grounded_ = false;
    bool jumped_ = false;
    float air_time_ = 0.0f;
    float jump_buffer_timer_ = 0.0f;
    float yaw_deg_ = 0.0f;
    bool facing_override_ = false;
    glm::vec3 ground_normal_{0.0f, 0.0f, 1.0f};
    coopa::physx::dynamics::BodyId ground_body_;   ///< What we stood on last frame...
    glm::mat4 ground_world_{1.0f};                 ///< ...and its world matrix then.
    glm::vec3 root_motion_{0.0f};
    float root_yaw_ = 0.0f;
    coopa::physx::character::MoveResult last_move_;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CHARACTER_CONTROLLER_H
