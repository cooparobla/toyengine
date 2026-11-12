/**
 * @file ragdoll.h
 * @brief Ragdoll -- turns an animated rig into jointed physics bodies and back: bones follow
 *        their SkeletalAnimator pose as kinematic bodies (so hits register), go limp under
 *        physics on activate(), and blend from wherever they fell back into an animation state
 *        on deactivate().
 *
 * Put it on the rig ROOT (the object carrying the Rig and SkeletalAnimator). Each entry of
 * `bones` names a bone of that rig and gives it a collision shape, a mass and a joint to the
 * nearest ANCESTOR bone in the list (the first, top-most bone -- usually the pelvis -- has
 * none). Since a bone is not itself a SceneObject, start() creates one flat physics PROXY
 * object per listed bone (a child of the Ragdoll's own object, named "_ragdoll_<bone>"), each
 * carrying a BoneAttachment (so it rides the bone while animated), a RigidbodyComponent and a
 * Capsule/Box/SphereCollider. The joints are created straight in the physics world once those
 * bodies exist, from the rig's REST pose (toy::rig::Rig::rest_model_matrix -- unaffected by
 * whatever clip is playing), so limits are measured the same way no matter when the ragdoll
 * activates.
 *
 * Modes:
 *   - Animated: a bone's proxy is kinematic, carried by its BoneAttachment (which itself reads
 *     the SkeletalAnimator's pose through the Rig). Physics sees it -- a raycast hits an arm, a
 *     ball bounces off a leg -- but cannot move it.
 *   - Ragdoll: proxies are dynamic, the SkeletalAnimator is cleared (SkeletalAnimator::clear(),
 *     so nothing re-applies a pose over the physics), IK under the rig is suspended, and physics
 *     writes the proxies' Transforms directly (PhysicsSystem's write-back) -- the BoneAttachments
 *     are left in place but idle, since nothing drives the Rig's pose while limp so there is
 *     nothing for them to apply. Once the body has finished moving it is put to sleep
 *     (`rest_travel`); a later blow, or anything touching it, takes it up again.
 *   - Blending (after deactivate()): proxies are kinematic again and the SkeletalAnimator plays
 *     the recovery state; each frame every bone's world pose is blended from where the ragdoll
 *     left it to the freshly animated pose and written directly into the Rig's pose channels
 *     (so the skinned mesh follows) and onto each proxy's Transform (so its collider follows
 *     too), over `blend_time`, then the mode returns to Animated.
 *
 * With a CharacterController on the root (and `link_character`), activation suspends it (its
 * capsule stops colliding, it stops moving) and recovery teleports it under the pelvis, facing
 * the way the body lies, before resuming it. Without one, recovery moves the root the same way
 * (`reposition_root`), so the rig stands up where it fell rather than sliding back to where it
 * started.
 *
 * Three system stages drive it (install_ragdoll_system()): order 90, before Physics (100) --
 * joint creation, mode changes, impulses; order 290, before Animation (300) -- during a blend,
 * clears the ragdoll's own bones' pose channels to identity (the rest pose) so the
 * SkeletalAnimator's output is a complete target pose; and order 330, after Animation and IK
 * (320) -- writes the blended pose.
 *
 * `auto_generate: true` (with no `bones`) builds the list itself: every bone of the rig becomes
 * a ragdoll bone, with a capsule along its own rest head-to-tail axis (local +Y, by the rig's
 * own convention) or, for a bone too short to carry one, a small sphere; mass split by volume,
 * and a cone-twist joint to its parent bone.
 *
 * Example YAML (see assets/objects/characters/mannequin_ragdoll.yaml for a full humanoid):
 * @code
 * - type: Ragdoll
 *   mode: animated            # or ragdoll
 *   blend_time: 0.5
 *   recover_state: idle
 *   bones:                    # angles in degrees; the joint keys describe the joint to the parent bone
 *     - {bone: pelvis, shape: capsule, radius: 0.12, height: 0.36, direction: x, mass: 12}
 *     - bone: thigh_l
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
 *     - {bone: shin_l, joint: hinge, axis: {x: 1, y: 0, z: 0}, limit_min: -140, limit_max: 0}
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

#include <toyengine/rig/bone_attachment.h>
#include <toyengine/rig/rig_component.h>
#include <toyengine/rig/skeletal_animator.h>

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

    std::string bone;               ///< Bone name within the rig (e.g. "thigh_l").

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
    /**
     * @brief Rest assist: a limp body whose every bone has stayed within `rest_travel` of where
     *        it was for `rest_time` seconds has finished moving, and is put to sleep as a whole.
     *        rest_time <= 0 turns it off. Anything touching the body wakes it again, as does a
     *        blow (activate() on a body already limp).
     *
     * Travelled, not speed: contacts and joints leave a settled ragdoll permanently twitching at
     * a few cm/s -- below anything visible, above the world's own per-body sleep thresholds, and
     * above any instantaneous threshold low enough to mean "stopped". Watching speed, one bone's
     * twitch restarts the whole body's clock every few frames, so it never sleeps and never
     * stops twitching; watching where the bones actually ARE answers the question the twitch
     * obscures, since a body going nowhere IS at rest however busy its contacts look.
     *
     * 2 cm over a second: under what a lying body visibly moves, well over the jitter.
     */
    float rest_travel = 0.02f;
    float rest_time = 1.0f;
    /**
     * @brief Speed limits (m/s, rad/s) on a limp body's bones; 0 or less lifts them.
     *
     * Enforced on going limp and every frame after it, and both halves matter:
     *
     * - **Going limp**, a bone carries over the velocity it was animated with -- which is
     *   derived from how far its Transform moved last frame, so a crossfade, a clip that cut to
     *   a new pose, or the character having just been placed somewhere reads as metres in one
     *   frame and hands the body a speed nothing physical could have. That is a corpse fired
     *   off the map by its own death animation.
     * - **While limp**, the cap is what keeps a bone from tunnelling. Contacts are generated
     *   from where a body IS, not from the path it took, so a bone that covers more than its
     *   own thickness in one substep can pass clean through the ground -- and the far side of a
     *   streamed terrain surface is open space, with nothing to push it back. At 8 m/s a 1/60 s
     *   substep is 13 cm, under the size of any bone shape worth giving a body.
     *
     * 8 m/s is a sprint: faster than a body falls in the half-second it takes to hit the
     * ground from standing, and well past what any death throw needs to read as violent.
     */
    float max_limp_speed = 8.0f;
    float max_limp_spin = 12.0f;

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
    /** @brief The bone name bone `i` (as listed in `bones`) addresses -- NOT bone_object(i)'s own
     *         name, which is its physics proxy's ("_ragdoll_<name>"). */
    const std::string& bone_name(size_t i) const {
        static const std::string empty;
        return i < rt_.size() ? bones[rt_[i].spec].bone : empty;
    }
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
        coopa::scene::SceneObject* obj = nullptr;  ///< The bone's physics proxy object (see the file doc).
        coopa::physx::components::RigidbodyComponent* rb = nullptr;
        int bone_idx = -1;                     ///< Index into the rig's bones.
        int parent = -1;                       ///< Index into rt_.
        size_t spec = 0;                       ///< Index into bones.
        Pose bind_world;                       ///< The bone's REST pose, world space.
        glm::vec3 scale{1.0f};                 ///< World scale at bind.
        glm::vec3 com{0.0f};                   ///< Body centre from the pivot, bone frame, scaled.
        coopa::physx::dynamics::JointId joint;
        Pose from;                             ///< Blend start (world).
    };

    static Pose world_pose_(const coopa::scene::SceneObject* obj);

    /** @brief Sets bone i's pose channel so its resolved WORLD pose becomes exactly (pos, rot),
     *         by composing a delta onto whatever it currently resolves to -- see
     *         toyengine/rig/rig_pose_math.h's apply_world_rotation_delta/apply_world_translation_delta.
     *         Must be called parent-before-child (rt_ is kept in that order) so a child's "current
     *         world pose" already reflects its parent's just-written target. */
    void set_bone_world_pose_(int bone_idx, const glm::vec3& pos, const glm::quat& rot);

    /** @brief Finds or creates bone `spec`'s physics proxy object (a flat child of owner, see
     *         the file doc), with a BoneAttachment bound to it. */
    coopa::scene::SceneObject* proxy_object_(const RagdollBone& spec);

    /** @brief auto_generate: every rig bone becomes a ragdoll bone (see the file doc). */
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

    toy::rig::SkeletalAnimator* animator_() const {
        return owner ? owner->get_component<toy::rig::SkeletalAnimator>() : nullptr;
    }

    /** @brief IK under the rig off (FootIK suspended, TwoBoneIK / LookAtIK weights zeroed) or
     *         back on (weights restored). */
    void set_ik_suspended_(bool suspended);

    /**
     * @brief Suspends (or resumes) every bone's proxy BoneAttachment. Limp, physics owns the
     *        proxies' Transforms exclusively -- BoneAttachment must not also keep re-deriving
     *        them from the rig's now-frozen pose each frame (see BoneAttachment::suspended's
     *        own doc for what goes wrong if it does). Resumed once a recovery blend ends and
     *        the mode is Animated again; stays suspended through Blending too, since
     *        write_world_poses_() is itself keeping the Rig's pose and the proxies' Transforms
     *        in agreement for that whole span.
     */
    void set_bones_suspended_(bool suspended);

    void go_limp_(coopa::physx::system::PhysicsSystem& physics);

    /** @brief Holds every bone under `max_limp_speed` / `max_limp_spin`. */
    void limit_limp_speed_(coopa::physx::system::PhysicsSystem& physics);

    /** @brief Spends activate()'s impulse over the bones (at a point, or by mass). */
    void apply_pending_impulse_(coopa::physx::system::PhysicsSystem& physics);

    /** @brief See `rest_travel`. */
    void rest_assist_(coopa::physx::system::PhysicsSystem& physics, float dt);

    void begin_recovery_(coopa::physx::system::PhysicsSystem& physics);

    /**
     * @brief Writes each bone's pose so its world pose is `world[i]`: into the Rig's pose
     *        channel (so the skinned mesh follows) and onto its physics proxy's Transform (so
     *        its collider follows too) -- parent bones first (rt_'s order), so a child bone's
     *        delta composes onto its parent's already-written target. See
     *        set_bone_world_pose_()'s doc for the pose-channel algebra.
     */
    void write_world_poses_(const std::vector<Pose>& world);

    toy::rig::Rig* rig_ = nullptr;
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
    /** @brief Where each bone's body was when the rest window began; empty until limp. */
    std::vector<glm::vec3> rest_anchor_;
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
