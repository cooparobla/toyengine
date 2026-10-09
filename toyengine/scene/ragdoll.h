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
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <coopa/animation/animator.h>
#include <coopa/animation/ik_components.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <coopa/scene/components/transform_component.h>

#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/dynamics/integrator.h>
#include <physxcoopa/dynamics/joint.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/foot_ik.h>

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
    void activate(const glm::vec3& impulse = glm::vec3(0.0f), std::optional<glm::vec3> point = std::nullopt) {
        pending_ = Request::Activate;
        pending_impulse_ = impulse;
        pending_point_ = point;
    }

    /**
     * @brief Recovers: blends from the ragdoll pose into Animator state `state` (empty =
     *        `recover_state`) over `blend` seconds (negative = `blend_time`). No-op unless limp.
     */
    void deactivate(float blend = -1.0f, const std::string& state = "") {
        pending_ = Request::Deactivate;
        pending_blend_ = blend;
        pending_state_ = state;
    }

    /** @brief The engine's input push (see `input_toggle`): flips between the two. */
    void toggle() {
        if (mode_ == Mode::Ragdoll || pending_ == Request::Activate) deactivate();
        else activate(toggle_impulse);
    }

    void start() override {
        if (!owner) return;
        build_();
        if (start_ragdoll) activate(start_impulse);
        if (scene) {
            if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"))) {
                physics->refresh(); // a ragdoll spawned into a running scene: bind its new bodies
            }
        }
    }

    // --- Stages (called by RagdollSystem; see the file doc) ---

    /** @brief Order 90: joints once the bodies exist, then any pending mode change. */
    void stage_pre_physics(float dt, coopa::physx::system::PhysicsSystem* physics) {
        if (!physics || rt_.empty()) return;
        if (!ready_) {
            if (!create_joints_(*physics)) return;
            ready_ = true;
        }
        const Request req = pending_;
        pending_ = Request::None;
        if (req == Request::Activate && mode_ != Mode::Ragdoll) go_limp_(*physics);
        else if (req == Request::Deactivate && mode_ == Mode::Ragdoll) begin_recovery_(*physics);
        else if (mode_ == Mode::Ragdoll) rest_assist_(*physics, dt);
    }

    /** @brief Order 290: during a blend, bind-pose the bones so the Animator's output is a
     *         complete pose (a channel no clip drives would otherwise keep last frame's blend). */
    void stage_pre_animation() {
        if (mode_ != Mode::Blending) return;
        for (BoneRt& b : rt_) {
            coopa::util::Transform& t = b.obj->get_transform()->transform();
            t.set_position(b.bind_local_pos);
            t.set_rotation_quat(b.bind_local_rot);
        }
    }

    /** @brief Order 330: during a blend, writes the blended pose; ends the blend. */
    void stage_post_animation(float dt) {
        if (mode_ != Mode::Blending) return;
        blend_t_ += std::max(dt, 0.0f);
        const float x = blend_dur_ > 0.0f ? std::clamp(blend_t_ / blend_dur_, 0.0f, 1.0f) : 1.0f;
        const float w = x * x * (3.0f - 2.0f * x);
        std::vector<Pose> target(rt_.size());
        for (size_t i = 0; i < rt_.size(); ++i) {
            const Pose anim = world_pose_(rt_[i].obj);
            target[i].position = glm::mix(rt_[i].from.position, anim.position, w);
            target[i].rotation = glm::slerp(rt_[i].from.rotation, anim.rotation, w);
        }
        write_world_poses_(target);
        if (x >= 1.0f) {
            mode_ = Mode::Animated;
            set_ik_suspended_(false);
        }
    }

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

    static Pose world_pose_(const coopa::scene::SceneObject* obj) {
        const glm::mat4 m = obj->get_transform()->transform().get_world_matrix();
        Pose p;
        glm::vec3 scale, skew;
        glm::vec4 persp;
        glm::decompose(m, scale, p.rotation, p.position, skew, persp);
        return p;
    }

    static int depth_(const coopa::scene::SceneObject* o) {
        int d = 0;
        for (; o; o = o->parent()) ++d;
        return d;
    }

    static bool only_transform_(const coopa::scene::SceneObject& o) {
        return o.components().size() == 1 && o.get_transform() != nullptr;
    }

    /** @brief Resolves `path` (slash-separated child names) under the owner. */
    coopa::scene::SceneObject* resolve_(const std::string& path) const {
        coopa::scene::SceneObject* cur = owner;
        size_t start = 0;
        while (cur && start <= path.size()) {
            size_t end = path.find('/', start);
            const std::string name = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!name.empty()) {
                coopa::scene::SceneObject* next = nullptr;
                for (const auto& c : cur->children()) {
                    if (c->name() == name) { next = c.get(); break; }
                }
                cur = next;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return cur == owner ? nullptr : cur;
    }

    std::string path_of_(const coopa::scene::SceneObject* o) const {
        std::string p;
        for (; o && o != owner; o = o->parent()) p = p.empty() ? o->name() : o->name() + "/" + p;
        return p;
    }

    /** @brief auto_generate: empties under the root are bones; capsules fitted between them. */
    void auto_generate_() {
        std::vector<coopa::scene::SceneObject*> joints;
        owner->for_each_recursive([&](const coopa::scene::SceneObject& o) {
            if (&o != owner && only_transform_(o)) joints.push_back(const_cast<coopa::scene::SceneObject*>(&o));
        });
        auto child_bones = [&](const coopa::scene::SceneObject* o) {
            std::vector<coopa::scene::SceneObject*> out;
            for (const auto& c : o->children()) {
                if (std::find(joints.begin(), joints.end(), c.get()) != joints.end()) out.push_back(c.get());
            }
            return out;
        };
        for (coopa::scene::SceneObject* j : joints) {
            RagdollBone b;
            b.bone = path_of_(j);
            const std::vector<coopa::scene::SceneObject*> kids = child_bones(j);
            glm::vec3 seg(0.0f);
            if (kids.size() == 1) {
                seg = kids[0]->get_transform()->transform().position();
            } else if (kids.size() > 1) {
                for (auto* k : kids) seg += k->get_transform()->transform().position();
                seg /= static_cast<float>(kids.size());
            } else if (j->parent()) {
                // A leaf (hand, foot, head): half its parent segment's length, onward.
                const glm::vec3 from_parent = j->get_transform()->transform().position();
                seg = from_parent * 0.5f;
            }
            float len = glm::length(seg);
            if (len < 0.05f) {
                // A hub (pelvis: children spread around it) -- a sphere covering their reach.
                float reach = 0.0f;
                for (auto* k : kids) reach = std::max(reach, glm::length(k->get_transform()->transform().position()));
                b.shape = RagdollBone::Shape::Sphere;
                b.radius = std::max(0.05f, reach * 0.6f);
            } else {
                // A capsule in the bone's frame, rotated onto `seg` by direction + an axis pick:
                // the dominant component of `seg` picks X/Y/Z (rigs are axis-aligned at bind).
                glm::vec3 a = glm::abs(seg);
                b.direction = (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
                b.radius = std::clamp(len * 0.22f, 0.03f, 0.2f);
                b.height = std::max(len, 2.0f * b.radius + 0.01f);
                b.center = seg * 0.5f;
                b.axis = glm::normalize(seg);
            }
            b.joint = RagdollBone::Joint::ConeTwist;
            bones.push_back(b);
        }
    }

    static float shape_volume_(const RagdollBone& b) {
        constexpr float k_pi = 3.14159265f;
        switch (b.shape) {
            case RagdollBone::Shape::Box: return b.size.x * b.size.y * b.size.z;
            case RagdollBone::Shape::Sphere: return (4.0f / 3.0f) * k_pi * b.radius * b.radius * b.radius;
            case RagdollBone::Shape::Capsule:
            default: {
                const float cyl = std::max(0.0f, b.height - 2.0f * b.radius);
                return k_pi * b.radius * b.radius * cyl + (4.0f / 3.0f) * k_pi * b.radius * b.radius * b.radius;
            }
        }
    }

    /** @brief Resolves the bones, records the bind pose, adds the Rigidbody + Collider. */
    void build_() {
        rt_.clear();
        if (bones.empty() && auto_generate) auto_generate_();
        if (bones.empty()) return;

        std::vector<BoneRt> list;
        for (size_t i = 0; i < bones.size(); ++i) {
            BoneRt b;
            b.obj = resolve_(bones[i].bone);
            if (!b.obj || !b.obj->get_transform()) {
                std::cerr << "[Ragdoll] '" << (owner ? owner->name() : std::string("?")) << "': bone '" << bones[i].bone
                          << "' not found -- skipped" << std::endl;
                continue;
            }
            b.spec = i;
            list.push_back(b);
        }
        // Parents before children (write_world_poses_() and the joints rely on it).
        std::stable_sort(list.begin(), list.end(),
                         [](const BoneRt& x, const BoneRt& y) { return depth_(x.obj) < depth_(y.obj); });

        float free_volume = 0.0f, fixed_mass = 0.0f;
        for (const BoneRt& b : list) {
            const RagdollBone& s = bones[b.spec];
            if (s.mass > 0.0f) fixed_mass += s.mass;
            else free_volume += shape_volume_(s);
        }
        const float spare_mass = std::max(mass - fixed_mass, 0.0f);

        for (size_t i = 0; i < list.size(); ++i) {
            BoneRt& b = list[i];
            for (const coopa::scene::SceneObject* p = b.obj->parent(); p && p != owner && b.parent < 0; p = p->parent()) {
                for (size_t k = 0; k < i; ++k) {
                    if (list[k].obj == p) { b.parent = static_cast<int>(k); break; }
                }
            }
            coopa::util::Transform& t = b.obj->get_transform()->transform();
            b.bind_local_pos = t.position();
            b.bind_local_rot = t.rotation_quat();
            const glm::mat4 m = t.get_world_matrix();
            glm::vec3 skew;
            glm::vec4 persp;
            glm::decompose(m, b.scale, b.bind_world.rotation, b.bind_world.position, skew, persp);

            const RagdollBone& s = bones[b.spec];
            b.com = s.center * b.scale;
            float body_mass = s.mass;
            if (body_mass <= 0.0f) {
                body_mass = free_volume > 0.0f ? spare_mass * shape_volume_(s) / free_volume : 1.0f;
                body_mass = std::max(body_mass, 0.2f);
            }

            using namespace coopa::physx::components;
            b.rb = b.obj->get_component<RigidbodyComponent>();
            if (!b.rb) b.rb = b.obj->add_component<RigidbodyComponent>();
            b.rb->scene = scene;
            b.rb->mass = body_mass;
            b.rb->is_kinematic = true;   // Animated until the joints exist (see stage_pre_physics())
            b.rb->use_gravity = true;
            b.rb->drag = drag;
            b.rb->angular_drag = angular_drag;
            Collider* col = nullptr;
            switch (s.shape) {
                case RagdollBone::Shape::Box: {
                    auto* c = b.obj->add_component<BoxCollider>();
                    c->set_size(s.size);
                    col = c;
                    break;
                }
                case RagdollBone::Shape::Sphere: {
                    auto* c = b.obj->add_component<SphereCollider>();
                    c->set_radius(s.radius);
                    col = c;
                    break;
                }
                case RagdollBone::Shape::Capsule:
                default: {
                    auto* c = b.obj->add_component<CapsuleCollider>();
                    c->set_radius(s.radius);
                    c->set_height(s.height);
                    c->set_direction(std::clamp(s.direction, 0, 2));
                    col = c;
                    break;
                }
            }
            col->scene = scene;
            col->set_center(s.center);
            col->set_layer(layer);
        }
        rt_ = std::move(list);
        ready_ = false;
        mode_ = Mode::Animated;
    }

    /** @brief Creates one joint per non-root bone, from the bind pose. False until every bone
     *         has a body. */
    bool create_joints_(coopa::physx::system::PhysicsSystem& physics) {
        namespace dyn = coopa::physx::dynamics;
        coopa::physx::PhysicsWorld& world = physics.world();
        for (const BoneRt& b : rt_) {
            if (!b.rb || !world.is_valid(b.rb->body_id())) return false;
        }
        for (BoneRt& b : rt_) {
            if (b.parent < 0) continue;
            const BoneRt& p = rt_[static_cast<size_t>(b.parent)];
            const RagdollBone& s = bones[b.spec];
            const glm::quat rp = p.bind_world.rotation, rb = b.bind_world.rotation;
            const glm::vec3 body_p = p.bind_world.position + rp * p.com;
            const glm::vec3 body_b = b.bind_world.position + rb * b.com;
            const glm::vec3 anchor = b.bind_world.position + rb * (s.anchor * b.scale);

            glm::vec3 axis = s.axis;
            if (glm::length(axis) < 1e-6f) {
                axis = glm::length(s.center) > 1e-4f ? s.center : glm::vec3(0.0f);
                if (glm::length(axis) < 1e-6f) axis[std::clamp(s.direction, 0, 2)] = 1.0f;
            }
            axis = glm::normalize(axis);
            const glm::vec3 world_axis = rb * axis;

            dyn::Joint j;
            j.type = s.joint == RagdollBone::Joint::Hinge ? dyn::JointType::Hinge
                   : s.joint == RagdollBone::Joint::Ball ? dyn::JointType::Ball
                                                         : dyn::JointType::ConeTwist;
            j.a = p.rb->body_id();
            j.b = b.rb->body_id();
            j.local_anchor_a = glm::inverse(rp) * (anchor - body_p);
            j.local_anchor_b = glm::inverse(rb) * (anchor - body_b);
            j.local_axis_a = glm::inverse(rp) * world_axis;
            j.local_axis_b = axis;
            j.rest_relative_rotation = glm::inverse(rp) * rb;
            j.collide_connected = collide_connected;
            if (j.type == dyn::JointType::Hinge) {
                j.use_limits = true;
                j.min_angle = glm::radians(s.hinge_min_deg);
                j.max_angle = glm::radians(s.hinge_max_deg);
            } else if (j.type == dyn::JointType::ConeTwist) {
                j.swing_limit = s.swing_deg >= 0.0f ? glm::radians(s.swing_deg) : -1.0f;
                j.twist_min = glm::radians(s.twist_min_deg);
                j.twist_max = glm::radians(s.twist_max_deg);
            }
            b.joint = world.add_joint(j);
        }
        // Near bones overlap by design around a joint they do not share (the spine and the
        // upper arms at the shoulders, the two thighs at the hips): contacts there would fight
        // the joints forever and keep the body from ever resting. Bones within two links of
        // each other (grandparent / sibling) never collide; the rest still do.
        auto chain = [&](size_t i) {
            std::vector<int> up;
            for (int k = static_cast<int>(i); k >= 0; k = rt_[static_cast<size_t>(k)].parent) up.push_back(k);
            return up;
        };
        for (size_t i = 0; i < rt_.size(); ++i) {
            const std::vector<int> ci = chain(i);
            for (size_t k = i + 1; k < rt_.size(); ++k) {
                const std::vector<int> ck = chain(k);
                int dist = 1 << 20;
                for (size_t a = 0; a < ci.size(); ++a) {
                    for (size_t c = 0; c < ck.size(); ++c) {
                        if (ci[a] == ck[c]) dist = std::min(dist, static_cast<int>(a + c));
                    }
                }
                if (dist <= 2) world.set_pair_collision(rt_[i].rb->body_id(), rt_[k].rb->body_id(), false);
            }
        }
        return true;
    }

    CharacterController* character_() const {
        return (link_character && owner) ? owner->get_component<CharacterController>() : nullptr;
    }

    coopa::anim::Animator* animator_() const {
        return owner ? owner->get_component<coopa::anim::Animator>() : nullptr;
    }

    /** @brief IK under the rig off (FootIK suspended, TwoBoneIK / LookAtIK weights zeroed) or
     *         back on (weights restored). */
    void set_ik_suspended_(bool suspended) {
        if (suspended == ik_suspended_) return;
        ik_suspended_ = suspended;
        if (suspended) saved_ik_weights_.clear();
        size_t k = 0;
        owner->for_each_recursive([&](const coopa::scene::SceneObject& o) {
            for (const auto& comp : o.components()) {
                if (auto* f = dynamic_cast<FootIK*>(comp.get())) {
                    f->set_suspended(suspended);
                } else if (auto* tb = dynamic_cast<coopa::anim::TwoBoneIK*>(comp.get())) {
                    if (suspended) { saved_ik_weights_.push_back(tb->weight); tb->weight = 0.0f; }
                    else if (k < saved_ik_weights_.size()) tb->weight = saved_ik_weights_[k++];
                } else if (auto* la = dynamic_cast<coopa::anim::LookAtIK*>(comp.get())) {
                    if (suspended) { saved_ik_weights_.push_back(la->weight); la->weight = 0.0f; }
                    else if (k < saved_ik_weights_.size()) la->weight = saved_ik_weights_[k++];
                }
            }
        });
    }

    void go_limp_(coopa::physx::system::PhysicsSystem& physics) {
        namespace dyn = coopa::physx::dynamics;
        coopa::physx::PhysicsWorld& world = physics.world();
        if (auto* a = animator_()) {
            if (recover_state.empty()) recover_state = !a->auto_play.empty() ? a->auto_play : a->current_state();
            a->clear();
        }
        set_ik_suspended_(true);
        if (CharacterController* cc = character_()) cc->set_suspended(true);

        float total = 0.0f;
        for (BoneRt& b : rt_) {
            b.rb->is_kinematic = false;
            const dyn::BodyId id = b.rb->body_id();
            world.set_body_type(id, dyn::BodyType::Dynamic, b.rb->mass);
            if (dyn::Body* body = world.get_body(id)) {
                dyn::update_world_inertia(*body);
                body->wake();
                total += body->mass;
            }
        }
        if (glm::length(pending_impulse_) > 0.0f) {
            if (pending_point_) {
                dyn::Body* best = nullptr;
                float best_d = 1e30f;
                for (BoneRt& b : rt_) {
                    dyn::Body* body = world.get_body(b.rb->body_id());
                    if (!body) continue;
                    const float d = glm::length(body->position - *pending_point_);
                    if (d < best_d) { best_d = d; best = body; }
                }
                if (best) best->apply_impulse_at_position(pending_impulse_, *pending_point_);
            } else if (total > 0.0f) {
                for (BoneRt& b : rt_) {
                    if (dyn::Body* body = world.get_body(b.rb->body_id())) {
                        body->apply_impulse(pending_impulse_ * (body->mass / total));
                    }
                }
            }
        }
        pending_impulse_ = glm::vec3(0.0f);
        pending_point_.reset();
        mode_ = Mode::Ragdoll;
    }

    /** @brief See `rest_speed`. */
    void rest_assist_(coopa::physx::system::PhysicsSystem& physics, float dt) {
        if (rest_time <= 0.0f) return;
        coopa::physx::PhysicsWorld& world = physics.world();
        bool any_awake = false, calm = true;
        for (BoneRt& b : rt_) {
            const coopa::physx::dynamics::Body* body = world.get_body(b.rb->body_id());
            if (!body || !body->awake) continue;
            any_awake = true;
            calm = calm && glm::length(body->linear_velocity) < rest_speed && glm::length(body->angular_velocity) < rest_spin;
        }
        if (!any_awake || !calm) {
            rest_timer_ = 0.0f;
            return;
        }
        rest_timer_ += std::max(dt, 0.0f);
        if (rest_timer_ < rest_time) return;
        for (BoneRt& b : rt_) {
            if (coopa::physx::dynamics::Body* body = world.get_body(b.rb->body_id())) body->sleep();
        }
        rest_timer_ = 0.0f;
    }

    void begin_recovery_(coopa::physx::system::PhysicsSystem& physics) {
        namespace dyn = coopa::physx::dynamics;
        coopa::physx::PhysicsWorld& world = physics.world();
        for (BoneRt& b : rt_) b.from = world_pose_(b.obj);

        // Stand the root up under the pelvis (the top bone), facing the way the body lies.
        const BoneRt& top = rt_.front();
        const glm::vec3 pelvis = top.from.position;
        glm::vec3 ground = pelvis - glm::vec3(0.0f, 0.0f, top.bind_local_pos.z);
        {
            coopa::physx::system::PhysicsSystem::QueryFilter filter;
            filter.include_triggers = false;
            filter.predicate = [self = owner](const coopa::physx::components::Collider& c) {
                for (const coopa::scene::SceneObject* o = c.owner; o; o = o->parent())
                    if (o == self) return false;
                return true;
            };
            coopa::physx::geometry::Ray ray;
            ray.origin = pelvis + glm::vec3(0.0f, 0.0f, 0.5f);
            ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
            ray.max_distance = 4.0f;
            coopa::physx::system::PhysicsSystem::RaycastHit hit;
            if (physics.raycast(ray, hit, filter)) ground = hit.point;
        }
        glm::vec3 fwd = top.from.rotation * glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::length(glm::vec2(fwd.x, fwd.y)) < 0.3f) {
            // Lying face up/down: the forward axis points at the sky -- face along the spine.
            fwd = top.from.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
        }
        const float yaw = glm::length(glm::vec2(fwd.x, fwd.y)) > 1e-4f ? glm::degrees(std::atan2(-fwd.x, fwd.y)) : 0.0f;
        const glm::vec3 root_pos(pelvis.x, pelvis.y, ground.z);

        if (CharacterController* cc = character_()) {
            cc->teleport(root_pos, yaw);
            cc->set_suspended(false);
        } else if (reposition_root && owner->get_transform() && !owner->parent()) {
            coopa::util::Transform& t = owner->get_transform()->transform();
            t.set_position(root_pos);
            t.set_rotation(glm::vec3(0.0f, 0.0f, yaw));
        }

        for (BoneRt& b : rt_) {
            b.rb->is_kinematic = true;
            world.set_body_type(b.rb->body_id(), dyn::BodyType::Kinematic);
        }
        // Hold the ragdoll pose under the moved root now, so this frame's physics step sees the
        // bones where they lay (zero kinematic velocity, nothing knocked away).
        std::vector<Pose> hold(rt_.size());
        for (size_t i = 0; i < rt_.size(); ++i) hold[i] = rt_[i].from;
        write_world_poses_(hold);

        if (auto* a = animator_()) {
            const std::string state = !pending_state_.empty() ? pending_state_ : recover_state;
            if (!state.empty() && a->find_state(state)) a->play(state);
        }
        blend_dur_ = pending_blend_ >= 0.0f ? pending_blend_ : blend_time;
        blend_t_ = 0.0f;
        mode_ = Mode::Blending;
    }

    /**
     * @brief Writes each bone's LOCAL pose so its world pose is `world[i]` -- parents first,
     *        each bone's new parent matrix derived from its nearest bone ancestor's target (the
     *        same `ancestor_new * inverse(ancestor_old) * parent_old` composition as
     *        PhysicsSystem's write-back, so non-bone objects between two bones are covered).
     */
    void write_world_poses_(const std::vector<Pose>& world) {
        const size_t n = rt_.size();
        std::vector<glm::mat4> old_world(n), new_world(n), parent_old(n);
        for (size_t i = 0; i < n; ++i) {
            const coopa::util::Transform& t = rt_[i].obj->get_transform()->transform();
            old_world[i] = t.get_world_matrix();
            parent_old[i] = t.parent() ? t.parent()->get_world_matrix() : glm::mat4(1.0f);
            const glm::vec3 scale(glm::length(glm::vec3(old_world[i][0])), glm::length(glm::vec3(old_world[i][1])),
                                  glm::length(glm::vec3(old_world[i][2])));
            new_world[i] = glm::translate(glm::mat4(1.0f), world[i].position) * glm::mat4_cast(world[i].rotation) *
                           glm::scale(glm::mat4(1.0f), scale);
        }
        for (size_t i = 0; i < n; ++i) {
            coopa::util::Transform& t = rt_[i].obj->get_transform()->transform();
            glm::mat4 parent_world = parent_old[i];
            if (rt_[i].parent >= 0) {
                const size_t a = static_cast<size_t>(rt_[i].parent);
                parent_world = new_world[a] * glm::inverse(old_world[a]) * parent_old[i];
            }
            const glm::mat4 local = glm::inverse(parent_world) *
                                    (glm::translate(glm::mat4(1.0f), world[i].position) * glm::mat4_cast(world[i].rotation));
            glm::vec3 pos, scale, skew;
            glm::quat rot;
            glm::vec4 persp;
            glm::decompose(local, scale, rot, pos, skew, persp);
            t.set_position(pos);
            t.set_rotation_quat(rot);
        }
    }

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

    const char* system_name() const override {
        switch (stage_) {
            case Stage::PrePhysics: return "RagdollPrePhysics";
            case Stage::PreAnimation: return "RagdollPreAnimation";
            case Stage::PostAnimation:
            default: return "RagdollPostAnimation";
        }
    }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        std::vector<Ragdoll*> ragdolls = scene.get_components<Ragdoll>();
        if (ragdolls.empty()) return;
        switch (stage_) {
            case Stage::PrePhysics: {
                auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
                for (Ragdoll* r : ragdolls) r->stage_pre_physics(ctx.delta_time, physics);
                break;
            }
            case Stage::PreAnimation:
                for (Ragdoll* r : ragdolls) r->stage_pre_animation();
                break;
            case Stage::PostAnimation:
                for (Ragdoll* r : ragdolls) r->stage_post_animation(ctx.delta_time);
                break;
        }
    }

private:
    Stage stage_;
};

/**
 * @brief Installs the three RagdollSystem stages (orders 90, 290, 330). A scene with no Ragdoll
 *        pays three empty component sweeps per frame.
 */
inline void install_ragdoll_system(coopa::scene::Scene& scene) {
    scene.add_system(std::make_unique<RagdollSystem>(RagdollSystem::Stage::PrePhysics), k_ragdoll_pre_physics_order);
    scene.add_system(std::make_unique<RagdollSystem>(RagdollSystem::Stage::PreAnimation), k_ragdoll_pre_animation_order);
    scene.add_system(std::make_unique<RagdollSystem>(RagdollSystem::Stage::PostAnimation), k_ragdoll_post_animation_order);
}

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_RAGDOLL_H
