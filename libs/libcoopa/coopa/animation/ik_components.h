/**
 * @file ik_components.h
 * @brief IK components solved by IkSystem (ik_system.h) after the Animation phase: TwoBoneIK
 *        (a limb reaching for a target with a pole), ChainIK (a chain of any length -- a spine, a
 *        tail -- reaching for a target, FABRIK), LookAtIK (a bone turning to face a target,
 *        clamped), and IkDriver, the base for components that steer them each frame (toyengine's
 *        FootIK).
 *
 * All bone / target paths resolve the way an AnimationTrack's object_path does, relative to the
 * component's owner: empty = the owner, "a/b/c" walks descendants, a bare name is looked up as a
 * descendant then scene-wide.
 *
 * IK overrides the animated pose: it reads each bone's local rotation as the Animator left it
 * this frame, solves, and writes the result. A bone NO clip animates would otherwise feed last
 * frame's IK output back in as this frame's input; each solved bone therefore remembers what it
 * wrote, and when it finds that value still there it restores the pre-IK rotation first
 * (IkPoseGuard) -- so a weight of 0 always returns the bone exactly to its unsolved pose.
 *
 * Solve order: every solver has a `priority` (lower solves first; default 0), and solvers of
 * equal priority run chains first, then two-bone limbs, then look-ats, each in scene pre-order --
 * a spine settles before the arms hanging from it, and a head turn rides on both. A rig whose
 * solvers depend on each other some other way sets priorities.
 *
 * A FULLY IK rig is the same components at weight 1 with every limb and the spine driven: the
 * Animator then keys only the targets (empty objects), and IK poses every bone from them. Set
 * `match_target_rotation` so a hand / foot / chest takes its target's rotation, not just its
 * position (see objects/characters/mannequin_ik for the pattern).
 *
 * Example YAML:
 * @code
 * - type: TwoBoneIK
 *   upper: pelvis/spine/chest/upper_arm_r
 *   lower: pelvis/spine/chest/upper_arm_r/lower_arm_r
 *   end: pelvis/spine/chest/upper_arm_r/lower_arm_r/hand_r
 *   target: reach_target          # an object (any name); or set_target_position() from code
 *   pole: elbow_pole              # optional: the elbow bends toward this object
 *   weight: 1.0
 *   soft_limit: 0.03              # ease the last 3% of the reach
 *   match_target_rotation: false  # true: the end bone also takes the target's rotation
 * - type: ChainIK
 *   root: pelvis/spine            # the chain: root, its descendants down to tip
 *   tip: pelvis/spine/chest/neck  # the tip's origin reaches the target
 *   target: chest_target
 *   iterations: 10
 *   tolerance: 0.001
 *   match_target_rotation: true
 *   priority: 0                   # any solver: lower solves first
 * - type: LookAtIK
 *   bone: pelvis/spine/chest/neck/head
 *   target: look_target
 *   forward_axis: {x: 0, y: 1, z: 0}
 *   up_axis: {x: 0, y: 0, z: 1}
 *   max_angle: 70
 *   weight: 1.0
 *   smoothing: 0.1                # seconds (time constant); 0 = snap
 * @endcode
 */

#ifndef COOPA_ANIMATION_IK_COMPONENTS_H
#define COOPA_ANIMATION_IK_COMPONENTS_H

#include <coopa/animation/ik.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace coopa {
namespace anim {

/**
 * @brief Resolves a bone/target path relative to `owner` (see this file's doc).
 */
inline coopa::scene::SceneObject* resolve_ik_path(coopa::scene::SceneObject* owner, coopa::scene::Scene* scene,
                                                  const std::string& path) {
    if (!owner) return nullptr;
    if (path.empty()) return owner;
    if (path.find('/') != std::string::npos) {
        coopa::scene::SceneObject* cur = owner;
        size_t start = 0;
        while (cur) {
            const size_t slash = path.find('/', start);
            const std::string seg = slash == std::string::npos ? path.substr(start) : path.substr(start, slash - start);
            if (!seg.empty()) cur = cur->find_descendant(seg);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
        return cur;
    }
    if (coopa::scene::SceneObject* found = owner->find_descendant(path)) return found;
    return scene ? scene->find_object(path) : nullptr;
}

/** @brief World rotation of an object's parent (identity for a root). */
inline glm::quat ik_parent_world_rotation(coopa::scene::SceneObject* obj) {
    coopa::scene::SceneObject* p = obj ? obj->parent() : nullptr;
    auto* tc = p ? p->get_transform() : nullptr;
    return tc ? ik::rotation_of(tc->get_world_matrix()) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

/** @brief World position of an object (its Transform's origin). */
inline glm::vec3 ik_world_position(coopa::scene::SceneObject* obj) {
    auto* tc = obj ? obj->get_transform() : nullptr;
    return tc ? glm::vec3(tc->get_world_matrix()[3]) : glm::vec3(0.0f);
}

/**
 * @struct IkPoseGuard
 * @brief Remembers the local rotation an IK solve wrote to one bone, so an un-animated bone's
 *        pre-IK rotation can be restored the next frame instead of compounding (see file doc).
 */
struct IkPoseGuard {
    glm::quat written{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat input{1.0f, 0.0f, 0.0f, 0.0f};
    bool      valid = false;

    /** @brief Returns the bone's pre-IK local rotation, restoring it onto the Transform if the
     *         last solve's output is still in place. */
    glm::quat begin(coopa::util::Transform& t) {
        glm::quat cur = t.rotation_quat();
        if (valid && cur.x == written.x && cur.y == written.y && cur.z == written.z && cur.w == written.w) {
            cur = input;
            t.set_rotation_quat(cur);
        }
        input = cur;
        return cur;
    }

    /** @brief Writes the solved local rotation and remembers it. */
    void end(coopa::util::Transform& t, const glm::quat& q) {
        t.set_rotation_quat(q);
        written = t.rotation_quat();
        valid = true;
    }
};

/**
 * @class IkComponent
 * @brief Common base of every component IkSystem runs, so its per-frame scene walk costs one
 *        dynamic_cast per component.
 */
class IkComponent : public coopa::scene::Component {
public:
    enum class Kind { Driver, Chain, TwoBone, LookAt };
    virtual Kind ik_kind() const = 0;

    /// Order: lower first. Solvers -- ties: chains, then two-bone, then look-at. Drivers run
    /// their pre-/post-solves in this order too (ties: scene pre-order).
    int priority = 0;

    /** @brief One solve (solvers); drivers act through ik_pre_solve()/ik_post_solve() instead. */
    virtual void solve(float /*dt*/) {}
};

/**
 * @class IkDriver
 * @brief Base for a component that steers IK each frame (sets TwoBoneIK / LookAtIK targets,
 *        offsets a pelvis, ...). IkSystem calls ik_pre_solve() on every active driver before
 *        any chain is solved, and ik_post_solve() after all of them.
 */
class IkDriver : public IkComponent {
public:
    Kind ik_kind() const override { return Kind::Driver; }
    virtual void ik_pre_solve(float /*dt*/) {}
    virtual void ik_post_solve(float /*dt*/) {}
};

/**
 * @class TwoBoneIK
 * @brief Bends an upper/lower bone pair (thigh/shin, upper/lower arm) so the end bone's origin
 *        reaches a target, the middle joint bending toward an optional pole.
 */
class TwoBoneIK : public IkComponent {
public:
    std::string type_name() const override { return "TwoBoneIK"; }
    Kind ik_kind() const override { return Kind::TwoBone; }

    std::string upper;      ///< Hip / shoulder bone path.
    std::string lower;      ///< Knee / elbow bone path.
    std::string end;        ///< Ankle / wrist bone path (its origin is the end effector).
    std::string target;     ///< Target object path (unless set_target_position() is in use).
    std::string pole;       ///< Optional pole object path; empty keeps the animated bend direction.
    float weight = 1.0f;    ///< 0 = animation only, 1 = fully solved.
    float soft_limit = 0.02f; ///< Fraction of the chain length eased near full extension (ik::soft_reach).
    /// The end bone also takes the target's world rotation (a foot / hand keyed by its target).
    bool match_target_rotation = false;

    /** @brief Overrides `target` with a world position (cleared by clear_target_override()). */
    void set_target_position(const glm::vec3& p) { target_override_ = p; has_target_override_ = true; }
    /** @brief Overrides the target's world rotation (match_target_rotation; cleared with the position). */
    void set_target_rotation(const glm::quat& q) { target_rot_override_ = q; has_target_rot_override_ = true; }
    void clear_target_override() { has_target_override_ = false; has_target_rot_override_ = false; }

    /** @brief The target object (after the first solve or bind()); null when only overrides drive it. */
    coopa::scene::SceneObject* target_object() { if (!bound_) bind_(); return target_obj_; }
    /** @brief Overrides `pole` with a world position. */
    void set_pole_position(const glm::vec3& p) { pole_override_ = p; has_pole_override_ = true; }
    void clear_pole_override() { has_pole_override_ = false; }

    /** @brief The last solve's result (joint positions, reached flag). */
    const ik::TwoBoneResult& last_result() const { return result_; }

    /** @brief Resolved bones (after the first solve). */
    coopa::scene::SceneObject* upper_bone() const { return upper_obj_; }
    coopa::scene::SceneObject* lower_bone() const { return lower_obj_; }
    coopa::scene::SceneObject* end_bone() const { return end_obj_; }

    /** @brief Re-resolves the paths (after renaming or respawning bones). */
    void rebind() { bound_ = false; }
    /** @brief Resolves the paths now (they otherwise resolve on the first solve). */
    void bind() { if (!bound_) bind_(); }

    /**
     * @brief Puts an un-animated chain back to its pre-IK pose (the first thing solve() does).
     *        For a driver that reads the animated pose before choosing targets; idempotent
     *        within a frame.
     */
    void restore_input() {
        if (!owner) return;
        if (!bound_) bind_();
        if (upper_obj_ && upper_obj_->get_transform()) upper_guard_.begin(upper_obj_->get_transform()->transform());
        if (lower_obj_ && lower_obj_->get_transform()) lower_guard_.begin(lower_obj_->get_transform()->transform());
        if (match_target_rotation && end_obj_ && end_obj_->get_transform()) end_guard_.begin(end_obj_->get_transform()->transform());
    }

    /** @brief One solve. Main thread; called by IkSystem. */
    void solve(float /*dt*/) override {
        if (!owner) return;
        if (!bound_) bind_();
        if (!upper_obj_ || !lower_obj_ || !end_obj_) return;
        auto* ut = upper_obj_->get_transform();
        auto* lt = lower_obj_->get_transform();
        if (!ut || !lt) return;
        // Restore un-animated input first, so every position below is the animated pose's.
        const glm::quat upper_in = upper_guard_.begin(ut->transform());
        const glm::quat lower_in = lower_guard_.begin(lt->transform());
        auto* et = end_obj_->get_transform();
        const glm::quat end_in = (match_target_rotation && et) ? end_guard_.begin(et->transform()) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const float w = std::clamp(weight, 0.0f, 1.0f);
        glm::vec3 tgt;
        if (has_target_override_) tgt = target_override_;
        else if (target_obj_) tgt = ik_world_position(target_obj_);
        else return;
        if (w <= 0.0f) return;

        const glm::vec3 a = ik_world_position(upper_obj_);
        const glm::vec3 b = ik_world_position(lower_obj_);
        const glm::vec3 c = ik_world_position(end_obj_);
        const bool has_pole = has_pole_override_ || pole_obj_ != nullptr;
        const glm::vec3 pole_p = has_pole_override_ ? pole_override_ : (pole_obj_ ? ik_world_position(pole_obj_) : glm::vec3(0.0f));
        result_ = ik::solve_two_bone(a, b, c, tgt, pole_p, has_pole, soft_limit);

        const glm::quat up_delta = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), result_.upper_delta, w);
        upper_guard_.end(ut->transform(), ik::to_local_delta(ik_parent_world_rotation(upper_obj_), upper_in, up_delta));
        // The lower bone's parent chain just changed: re-solve its swing from the new pose, so a
        // partial weight still leaves the two bones consistent.
        const glm::vec3 b2 = ik_world_position(lower_obj_);
        const glm::vec3 c2 = ik_world_position(end_obj_);
        const glm::vec3 want_end = glm::mix(c, result_.end, w);
        glm::quat lo_delta(1.0f, 0.0f, 0.0f, 0.0f);
        if (glm::length(c2 - b2) > 1e-6f && glm::length(want_end - b2) > 1e-6f) {
            lo_delta = ik::rotation_between(glm::normalize(c2 - b2), glm::normalize(want_end - b2));
        }
        lower_guard_.end(lt->transform(), ik::to_local_delta(ik_parent_world_rotation(lower_obj_), lower_in, lo_delta));

        // The end bone's own rotation: the target's, blended by weight from the animated one.
        if (match_target_rotation && et) {
            const bool has_rot = has_target_rot_override_ || target_obj_ != nullptr;
            if (has_rot) {
                const glm::quat want = has_target_rot_override_ ? target_rot_override_ : ik::rotation_of(target_obj_->get_transform()->get_world_matrix());
                const glm::quat have = ik::rotation_of(et->get_world_matrix());
                const glm::quat delta = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::normalize(want * glm::inverse(have)), w);
                end_guard_.end(et->transform(), ik::to_local_delta(ik_parent_world_rotation(end_obj_), end_in, delta));
            }
        }
    }

private:
    void bind_() {
        upper_obj_ = resolve_ik_path(owner, scene, upper);
        lower_obj_ = resolve_ik_path(owner, scene, lower);
        end_obj_ = resolve_ik_path(owner, scene, end);
        target_obj_ = target.empty() ? nullptr : resolve_ik_path(owner, scene, target);
        pole_obj_ = pole.empty() ? nullptr : resolve_ik_path(owner, scene, pole);
        upper_guard_ = IkPoseGuard{};
        lower_guard_ = IkPoseGuard{};
        end_guard_ = IkPoseGuard{};
        bound_ = true;
    }

    bool bound_ = false;
    coopa::scene::SceneObject* upper_obj_ = nullptr;
    coopa::scene::SceneObject* lower_obj_ = nullptr;
    coopa::scene::SceneObject* end_obj_ = nullptr;
    coopa::scene::SceneObject* target_obj_ = nullptr;
    coopa::scene::SceneObject* pole_obj_ = nullptr;
    glm::vec3 target_override_{0.0f};
    glm::vec3 pole_override_{0.0f};
    glm::quat target_rot_override_{1.0f, 0.0f, 0.0f, 0.0f};
    bool has_target_override_ = false;
    bool has_target_rot_override_ = false;
    bool has_pole_override_ = false;
    IkPoseGuard upper_guard_;
    IkPoseGuard lower_guard_;
    IkPoseGuard end_guard_;
    ik::TwoBoneResult result_;
};

/**
 * @class ChainIK
 * @brief Bends a chain of any length -- a spine, a neck, a tail -- so the tip bone's origin
 *        reaches a target (FABRIK, ik::solve_chain). The chain is `root` and its descendants down
 *        to `tip`; every bone but the tip swings so it points at the next solved joint, and with
 *        `match_target_rotation` the tip takes the target's rotation too.
 *
 * Starting from the animated pose each frame, the solve keeps the chain's own bend wherever the
 * target leaves a choice. Twist about each bone is the input pose's (a chain carries no pole).
 */
class ChainIK : public IkComponent {
public:
    std::string type_name() const override { return "ChainIK"; }
    Kind ik_kind() const override { return Kind::Chain; }

    std::string root;       ///< First bone of the chain (it pivots; its origin stays put).
    std::string tip;        ///< Last bone: a descendant of `root`; its origin reaches the target.
    std::string target;     ///< Target object path (unless set_target_position() is in use).
    float weight = 1.0f;    ///< 0 = animation only, 1 = fully solved.
    int iterations = 10;    ///< FABRIK passes at most.
    float tolerance = 1e-3f;///< World units: close enough to stop.
    bool match_target_rotation = false;  ///< The tip also takes the target's world rotation.

    void set_target_position(const glm::vec3& p) { target_override_ = p; has_target_override_ = true; }
    void clear_target_override() { has_target_override_ = false; }

    /** @brief The chain's bones, root first (after the first solve). */
    const std::vector<coopa::scene::SceneObject*>& bones() const { return chain_; }
    /** @brief Whether the last solve put the tip on its target. */
    bool reached() const { return reached_; }
    void rebind() { bound_ = false; }

    /** @brief One solve. Main thread; called by IkSystem. */
    void solve(float /*dt*/) override {
        if (!owner) return;
        if (!bound_) bind_();
        if (chain_.size() < 2) return;
        // Restore un-animated input first, so every position below is the animated pose's.
        std::vector<glm::quat> local_in(chain_.size());
        for (size_t i = 0; i < chain_.size(); ++i) {
            const bool guarded = i + 1 < chain_.size() || match_target_rotation;
            auto* tc = chain_[i]->get_transform();
            local_in[i] = guarded ? guards_[i].begin(tc->transform()) : tc->transform().rotation_quat();
        }
        const float w = std::clamp(weight, 0.0f, 1.0f);
        glm::vec3 tgt;
        if (has_target_override_) tgt = target_override_;
        else if (target_obj_) tgt = ik_world_position(target_obj_);
        else return;
        if (w <= 0.0f) return;

        std::vector<glm::vec3> joints(chain_.size());
        for (size_t i = 0; i < chain_.size(); ++i) joints[i] = ik_world_position(chain_[i]);
        std::vector<glm::vec3> solved = joints;
        reached_ = ik::solve_chain(solved, tgt, iterations, tolerance);
        for (size_t i = 0; i < chain_.size(); ++i) solved[i] = glm::mix(joints[i], solved[i], w);

        // Parent to child: swing each bone so it points at the next solved joint, from the pose
        // as it stands after the bones above it moved.
        for (size_t i = 0; i + 1 < chain_.size(); ++i) {
            const glm::vec3 a = ik_world_position(chain_[i]);
            const glm::vec3 b = ik_world_position(chain_[i + 1]);
            glm::quat delta(1.0f, 0.0f, 0.0f, 0.0f);
            if (glm::length(b - a) > 1e-6f && glm::length(solved[i + 1] - a) > 1e-6f)
                delta = ik::rotation_between(glm::normalize(b - a), glm::normalize(solved[i + 1] - a));
            guards_[i].end(chain_[i]->get_transform()->transform(),
                           ik::to_local_delta(ik_parent_world_rotation(chain_[i]), local_in[i], delta));
        }
        if (match_target_rotation && target_obj_ && !has_target_override_) {
            auto* tt = chain_.back()->get_transform();
            const glm::quat want = ik::rotation_of(target_obj_->get_transform()->get_world_matrix());
            const glm::quat have = ik::rotation_of(tt->get_world_matrix());
            const glm::quat delta = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::normalize(want * glm::inverse(have)), w);
            guards_.back().end(tt->transform(), ik::to_local_delta(ik_parent_world_rotation(chain_.back()), local_in.back(), delta));
        }
    }

private:
    void bind_() {
        chain_.clear();
        coopa::scene::SceneObject* r = resolve_ik_path(owner, scene, root);
        coopa::scene::SceneObject* t = resolve_ik_path(owner, scene, tip);
        // tip up to root through the hierarchy; a tip that isn't under root binds nothing.
        for (coopa::scene::SceneObject* o = t; o; o = o->parent()) {
            chain_.push_back(o);
            if (o == r) break;
        }
        if (!r || chain_.empty() || chain_.back() != r) chain_.clear();
        std::reverse(chain_.begin(), chain_.end());
        for (auto* o : chain_) {
            if (!o->get_transform()) { chain_.clear(); break; }
        }
        target_obj_ = target.empty() ? nullptr : resolve_ik_path(owner, scene, target);
        guards_.assign(chain_.size(), IkPoseGuard{});
        bound_ = true;
    }

    bool bound_ = false;
    std::vector<coopa::scene::SceneObject*> chain_;
    coopa::scene::SceneObject* target_obj_ = nullptr;
    glm::vec3 target_override_{0.0f};
    bool has_target_override_ = false;
    std::vector<IkPoseGuard> guards_;
    bool reached_ = false;
};

/**
 * @class LookAtIK
 * @brief Turns one bone (a head, an eye, a turret) so its forward axis faces a target, at most
 *        `max_angle` away from its animated facing, with optional exponential smoothing.
 */
class LookAtIK : public IkComponent {
public:
    std::string type_name() const override { return "LookAtIK"; }
    Kind ik_kind() const override { return Kind::LookAt; }

    std::string bone;                              ///< Bone path; empty = the owner.
    std::string target;                            ///< Target object path (unless set_target_position()).
    glm::vec3 forward_axis{0.0f, 1.0f, 0.0f};      ///< The bone's local "looking" axis.
    glm::vec3 up_axis{0.0f, 0.0f, 1.0f};           ///< The bone's local up, kept from rolling.
    float max_angle = 70.0f;                       ///< Degrees from the animated facing.
    float weight = 1.0f;
    float smoothing = 0.0f;                        ///< Seconds (time constant); 0 = snap.

    void set_target_position(const glm::vec3& p) { target_override_ = p; has_target_override_ = true; }
    void clear_target_override() { has_target_override_ = false; }

    coopa::scene::SceneObject* bone_object() const { return bone_obj_; }
    void rebind() { bound_ = false; }

    /** @brief One solve. Main thread; called by IkSystem. */
    void solve(float dt) override {
        if (!owner) return;
        if (!bound_) {
            bone_obj_ = resolve_ik_path(owner, scene, bone);
            target_obj_ = target.empty() ? nullptr : resolve_ik_path(owner, scene, target);
            guard_ = IkPoseGuard{};
            smoothed_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            has_smoothed_ = false;
            bound_ = true;
        }
        if (!bone_obj_) return;
        auto* bt = bone_obj_->get_transform();
        if (!bt) return;
        const glm::quat local_in = guard_.begin(bt->transform());

        glm::quat desired(1.0f, 0.0f, 0.0f, 0.0f);
        const bool has_target = has_target_override_ || target_obj_ != nullptr;
        const float w = std::clamp(weight, 0.0f, 1.0f);
        if (has_target && w > 0.0f) {
            const glm::mat4 world = bt->get_world_matrix();
            const glm::quat wr = ik::rotation_of(world);
            const glm::vec3 pos(world[3]);
            const glm::vec3 tgt = has_target_override_ ? target_override_ : ik_world_position(target_obj_);
            const glm::vec3 to = tgt - pos;
            if (glm::dot(to, to) > 1e-10f && glm::dot(forward_axis, forward_axis) > 1e-10f) {
                const glm::vec3 f = glm::normalize(wr * glm::normalize(forward_axis));
                glm::vec3 u = glm::dot(up_axis, up_axis) > 1e-10f ? glm::normalize(wr * glm::normalize(up_axis)) : glm::vec3(0.0f, 0.0f, 1.0f);
                desired = ik::look_at_delta(f, u, glm::normalize(to), glm::radians(std::max(0.0f, max_angle)));
                desired = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), desired, w);
            }
        }
        if (smoothing > 0.0f && has_smoothed_) {
            const float k = 1.0f - std::exp(-std::max(dt, 0.0f) / smoothing);
            smoothed_ = glm::normalize(glm::slerp(smoothed_, desired, k));
        } else {
            smoothed_ = desired;
        }
        has_smoothed_ = true;
        guard_.end(bt->transform(), ik::to_local_delta(ik_parent_world_rotation(bone_obj_), local_in, smoothed_));
    }

    /** @brief The world-space delta applied last solve (identity = the animated pose). */
    const glm::quat& applied_delta() const { return smoothed_; }

private:
    bool bound_ = false;
    coopa::scene::SceneObject* bone_obj_ = nullptr;
    coopa::scene::SceneObject* target_obj_ = nullptr;
    glm::vec3 target_override_{0.0f};
    bool has_target_override_ = false;
    IkPoseGuard guard_;
    glm::quat smoothed_{1.0f, 0.0f, 0.0f, 0.0f};
    bool has_smoothed_ = false;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_IK_COMPONENTS_H
