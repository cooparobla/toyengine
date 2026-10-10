/**
 * @file ragdoll.h
 * @brief Ragdoll -- turns an animated rig into jointed physics bodies and back: bones follow the
 *        Animator as kinematic bodies (so hits register), go limp under physics on activate(),
 *        and blend from wherever they fell back into an animation state on deactivate().
 *
 * Put it on the rig ROOT (the object with the Animator). Each entry of `bones` names a bone by
 * path from the root and gives it a collision shape, a mass and a joint to the nearest ANCESTOR
 * bone in the list (the first, top-most bone -- usually the pelvis -- has none). start() adds a
 * RigidbodyComponent and a Capsule/Box/SphereCollider to each bone object; the joints are created
 * straight in the physics world once those bodies exist, from the rig's BIND pose (the pose the
 * bones have at start(), before any animation), so limits are measured from the authored rest
 * pose no matter which animation is playing when the ragdoll activates.
 *
 * Modes:
 *   - Animated: bones are kinematic and follow their Transforms (the Animator, IK). Physics sees
 *     them -- a raycast hits an arm, a ball bounces off a leg -- but cannot move them.
 *   - Ragdoll: bones are dynamic, the Animator is cleared (Animator::clear(), so nothing
 *     re-applies a pose over the physics), IK under the rig is suspended, and physics writes the
 *     bone Transforms (PhysicsSystem's hierarchical write-back).
 *   - Blending (after deactivate()): bones are kinematic again and the Animator plays the
 *     recovery state; each frame every bone's world pose is blended from where the ragdoll left
 *     it to the freshly animated pose over `blend_time`, then the mode returns to Animated.
 *
 * With a CharacterController on the root (and `link_character`), activation suspends it (its
 * capsule stops colliding, it stops moving) and recovery teleports it under the pelvis, facing
 * the way the body lies, before resuming it. Without one, recovery moves the root the same way
 * (`reposition_root`), so the rig stands up where it fell rather than sliding back to where it
 * started.
 *
 * Three system stages drive it (install_ragdoll_system()): order 90, before Physics (100) --
 * joint creation, mode changes, impulses; order 290, before Animation (300) -- during a blend,
 * puts the bones back to their bind pose so the Animator's output is a complete target pose; and
 * order 330, after Animation and IK (320) -- writes the blended pose.
 *
 * `auto_generate: true` (with no `bones`) builds the list itself: every empty descendant (an
 * object with nothing but a Transform -- the joints of an object-hierarchy or skinned rig) is a
 * bone, with a capsule from its pivot to its child bone (or, for a leaf, half its parent's
 * length onward), mass split by volume, and a cone-twist joint to its parent bone.
 *
 * Example YAML (see assets/objects/characters/mannequin_ragdoll.yaml for a full humanoid):
 * @code
 * - type: Ragdoll
 *   mode: animated            # or ragdoll
 *   blend_time: 0.5
 *   recover_state: idle
 *   bones:                    # angles in degrees; the joint keys describe the joint to the parent bone
 *     - {bone: pelvis, shape: capsule, radius: 0.12, height: 0.36, direction: x, mass: 12}
 *     - bone: pelvis/thigh_l
 *       shape: capsule
 *       radius: 0.08
 *       height: 0.43
 *       center: {x: 0, y: 0, z: -0.215}
 *       mass: 8
 *       joint: cone_twist      # cone_twist | hinge | ball
 *       axis: {x: 0, y: 0, z: -1}
 *       swing: 60
 *       twist_min: -30
 *       twist_max: 30
 *     - {bone: pelvis/thigh_l/shin_l, joint: hinge, axis: {x: 1, y: 0, z: 0}, limit_min: -140, limit_max: 0}
 * @endcode
 */

#ifndef TOYENGINE_SCENE_RAGDOLL_H
#define TOYENGINE_SCENE_RAGDOLL_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <coopa/animation/animator.h>


#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

/**
 * @struct RagdollBone
 * @brief One bone's body and its joint to the nearest ancestor bone (see the file doc).
 */
struct RagdollBone {
    enum class Shape { Capsule, Box, Sphere };
    enum class Joint { ConeTwist, Hinge, Ball };

    std::string bone;               ///< Path from the Ragdoll's object (e.g. "pelvis/thigh_l").

    Shape shape = Shape::Capsule;
    float radius = 0.08f;           ///< Capsule / sphere.
    float height = 0.3f;            ///< Capsule: total length, caps included (CapsuleCollider's).
    int direction = 2;              ///< Capsule axis in the bone's frame: 0 = X, 1 = Y, 2 = Z.
    glm::vec3 size{0.2f};           ///< Box: full extents.
    glm::vec3 center{0.0f};         ///< Shape centre in the bone's frame (from its pivot).
    float mass = 0.0f;              ///< kg; 0 = a share of Ragdoll::mass by volume.

    Joint joint = Joint::ConeTwist;
    /** @brief Joint position in the bone's frame -- 0 is the bone's pivot, where it meets its
     *         parent. */
    glm::vec3 anchor{0.0f};
    /** @brief Hinge axis, or the cone-twist twist axis, in the bone's frame. Zero = along the
     *         shape (from the pivot toward `center`, or the capsule's axis). */
    glm::vec3 axis{0.0f};
    float swing_deg = 45.0f;        ///< ConeTwist cone half-angle; negative = free.
    float twist_min_deg = -30.0f;   ///< ConeTwist twist range (min > max = free).
    float twist_max_deg = 30.0f;
    float hinge_min_deg = -90.0f;   ///< Hinge limits about `axis` (positive = right-handed).
    float hinge_max_deg = 90.0f;
};

/**
 * @class Ragdoll
 * @brief See the file doc.
 */
class Ragdoll : public coopa::scene::Component {
public:
    enum class Mode { Animated, Ragdoll, Blending };

    std::string type_name() const override { return "Ragdoll"; }

    std::vector<RagdollBone> bones;
    bool auto_generate = false;        ///< Build `bones` from the rig when it is empty.
    float mass = 70.0f;                ///< Total, split by volume over bones with mass 0.
    bool start_ragdoll = false;        ///< `mode: ragdoll` -- go limp as soon as the bodies exist.
    glm::vec3 start_impulse{0.0f};     ///< With `mode: ragdoll`: the activate() impulse (world, N s).
    bool collide_connected = false;    ///< Jointed bone pairs collide with each other.
    float drag = 0.05f;
    float angular_drag = 0.8f;         ///< Damps the floppy-limb jitter a light ragdoll is prone to.
    uint32_t layer = 0;                ///< Collider layer of every bone.
    float blend_time = 0.5f;           ///< Default deactivate() blend (s).
    std::string recover_state;         ///< Default deactivate() state; empty = the Animator's auto_play.
    bool link_character = true;        ///< Suspend/resume a CharacterController on this object.
    /** @brief Rest assist: a limp body whose every bone has stayed under these speeds (m/s,
     *         rad/s) for `rest_time` seconds is put to sleep as a whole. Contacts and joints
     *         leave a resting ragdoll creeping at a few mm/s -- below anything visible but above
     *         the world's per-body sleep thresholds -- so without this it never sleeps.
     *         rest_time <= 0 turns it off. Anything touching the body wakes it again. */
    float rest_speed = 0.08f;
    float rest_spin = 0.2f;
    float rest_time = 1.0f;
    bool reposition_root = true;       ///< Without a controller: move the root under the pelvis on recovery.
    /** @brief React to the engine's "ragdoll" input (R): activate() with `toggle_impulse` (world,
     *         N s, applied through the chest-height bones) when animated, deactivate() when limp. */
    bool input_toggle = false;
    glm::vec3 toggle_impulse{0.0f, 0.0f, 0.0f};

    // --- State ---
    Mode mode() const { return mode_; }
    bool is_ragdoll() const { return mode_ == Mode::Ragdoll; }
    /** @brief True once every bone has a body and the joints exist. */
    bool ready() const { return ready_; }
    /** @brief Seconds into the current recovery blend (Blending only). */
    float blend_elapsed() const { return blend_t_; }
    size_t bone_count() const { return rt_.size(); }
    coopa::scene::SceneObject* bone_object(size_t i) const { return i < rt_.size() ? rt_[i].obj : nullptr; }
    coopa::physx::dynamics::BodyId bone_body(size_t i) const {
        return i < rt_.size() && rt_[i].rb ? rt_[i].rb->body_id() : coopa::physx::dynamics::BodyId{};
    }
    coopa::physx::dynamics::JointId bone_joint(size_t i) const {
        return i < rt_.size() ? rt_[i].joint : coopa::physx::dynamics::JointId{};
    }
    /** @brief Index of bone `i`'s parent bone (its joint's other side), or -1. */
    int bone_parent(size_t i) const { return i < rt_.size() ? rt_[i].parent : -1; }

    /**
     * @brief Goes limp: bones become dynamic, keeping the velocity they were animated with, plus
     *        `impulse` (world, N s). With `point` the impulse hits the bone whose body is nearest
     *        to it, at that point; without, it is spread over every bone by mass. Taken at the
     *        next order-90 stage (before this frame's physics step); before the bodies exist it
     *        waits for them.
     */
    void activate(const glm::vec3& impulse = glm::vec3(0.0f), std::optional<glm::vec3> point = std::nullopt);

    /**
     * @brief Recovers: blends from the ragdoll pose into Animator state `state` (empty =
     *        `recover_state`) over `blend` seconds (negative = `blend_time`). No-op unless limp.
     */
    void deactivate(float blend = -1.0f, const std::string& state = "");

    /** @brief The engine's input push (see `input_toggle`): flips between the two. */
    void toggle();

    void start() override;

    // --- Stages (called by RagdollSystem; see the file doc) ---

    /** @brief Order 90: joints once the bodies exist, then any pending mode change. */
    void stage_pre_physics(float dt, coopa::physx::system::PhysicsSystem* physics);

    /** @brief Order 290: during a blend, bind-pose the bones so the Animator's output is a
     *         complete pose (a channel no clip drives would otherwise keep last frame's blend). */
    void stage_pre_animation();

    /** @brief Order 330: during a blend, writes the blended pose; ends the blend. */
    void stage_post_animation(float dt);

private:
    enum class Request { None, Activate, Deactivate };

    struct Pose {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    struct BoneRt {
        coopa::scene::SceneObject* obj = nullptr;
        coopa::physx::components::RigidbodyComponent* rb = nullptr;
        int parent = -1;                       ///< Index into rt_.
        size_t spec = 0;                       ///< Index into bones.
        glm::vec3 bind_local_pos{0.0f};
        glm::quat bind_local_rot{1.0f, 0.0f, 0.0f, 0.0f};
        Pose bind_world;
        glm::vec3 scale{1.0f};                 ///< World scale at bind.
        glm::vec3 com{0.0f};                   ///< Body centre from the pivot, bone frame, scaled.
        coopa::physx::dynamics::JointId joint;
        Pose from;                             ///< Blend start (world).
    };

    static Pose world_pose_(const coopa::scene::SceneObject* obj);

    static int depth_(const coopa::scene::SceneObject* o);

    static bool only_transform_(const coopa::scene::SceneObject& o) {
        return o.components().size() == 1 && o.get_transform() != nullptr;
    }

    /** @brief Resolves `path` (slash-separated child names) under the owner. */
    coopa::scene::SceneObject* resolve_(const std::string& path) const;

    std::string path_of_(const coopa::scene::SceneObject* o) const;

    /** @brief auto_generate: empties under the root are bones; capsules fitted between them. */
    void auto_generate_();

    static float shape_volume_(const RagdollBone& b);

    /** @brief Resolves the bones, records the bind pose, adds the Rigidbody + Collider. */
    void build_();

    /** @brief Creates one joint per non-root bone, from the bind pose. False until every bone
     *         has a body. */
    bool create_joints_(coopa::physx::system::PhysicsSystem& physics);

    CharacterController* character_() const {
        return (link_character && owner) ? owner->get_component<CharacterController>() : nullptr;
    }

    coopa::anim::Animator* animator_() const {
        return owner ? owner->get_component<coopa::anim::Animator>() : nullptr;
    }

    /** @brief IK under the rig off (FootIK suspended, TwoBoneIK / LookAtIK weights zeroed) or
     *         back on (weights restored). */
    void set_ik_suspended_(bool suspended);

    void go_limp_(coopa::physx::system::PhysicsSystem& physics);

    /** @brief See `rest_speed`. */
    void rest_assist_(coopa::physx::system::PhysicsSystem& physics, float dt);

    void begin_recovery_(coopa::physx::system::PhysicsSystem& physics);

    /**
     * @brief Writes each bone's LOCAL pose so its world pose is `world[i]` -- parents first,
     *        each bone's new parent matrix derived from its nearest bone ancestor's target (the
     *        same `ancestor_new * inverse(ancestor_old) * parent_old` composition as
     *        PhysicsSystem's write-back, so non-bone objects between two bones are covered).
     */
    void write_world_poses_(const std::vector<Pose>& world);

    std::vector<BoneRt> rt_;
    Mode mode_ = Mode::Animated;
    bool ready_ = false;
    bool ik_suspended_ = false;
    std::vector<float> saved_ik_weights_;
    Request pending_ = Request::None;
    glm::vec3 pending_impulse_{0.0f};
    std::optional<glm::vec3> pending_point_;
    float pending_blend_ = -1.0f;
    std::string pending_state_;
    float blend_t_ = 0.0f;
    float blend_dur_ = 0.5f;
    float rest_timer_ = 0.0f;
};

/** @brief The three Ragdoll stages' default orders (see the file doc). */
inline constexpr int k_ragdoll_pre_physics_order = 90;
inline constexpr int k_ragdoll_pre_animation_order = 290;
inline constexpr int k_ragdoll_post_animation_order = 330;

/**
 * @class RagdollSystem
 * @brief Runs one Ragdoll stage over every Ragdoll in the scene (one instance per stage).
 */
class RagdollSystem : public coopa::scene::ISceneSystem {
public:
    enum class Stage { PrePhysics, PreAnimation, PostAnimation };
    explicit RagdollSystem(Stage stage) : stage_(stage) {}

    const char* system_name() const override;

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

private:
    Stage stage_;
};

/**
 * @brief Installs the three RagdollSystem stages (orders 90, 290, 330). A scene with no Ragdoll
 *        pays three empty component sweeps per frame.
 */
void install_ragdoll_system(coopa::scene::Scene& scene);

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_RAGDOLL_H
