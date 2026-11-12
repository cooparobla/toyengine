/**
 * @file ik_components.h
 * @brief IK components solved by IkSystem (ik_system.h), in BONE space: TwoBoneIK, ChainIK and
 *        LookAtIK pose a Rig's bones toward scene-object targets. The math is
 *        toy::anim::ik's (pure, scene-free, shared with the property-animation IK) --
 *        only the thing being posed differs: a bone index in a Rig's pose buffer, not a
 *        SceneObject's Transform.
 *
 * Bones are named within the rig (`upper`, `lower`, ...); targets and poles remain ordinary
 * scene objects, resolved exactly like an AnimationTrack's object_path (see
 * toy::anim::resolve_ik_path, reused directly here).
 *
 * There is no IkPoseGuard here, unlike toy::anim::ik_components.h. That guard exists only
 * because a SceneObject's Transform is both an Animator's output AND an IK solver's
 * input/output, with no record of "did IK write this value last frame" other than comparing
 * bits. A Rig's pose channel is the SAME single-writer-per-frame state every reader (another
 * solver down the chain, RigPoseSystem) consumes already-composed with rest, so solve()
 * simply reads the Rig's CURRENT resolved pose each time it runs; a bone no clip or solver
 * touches already sits at identity (see Rig::clear_pose()), with nothing to feed back into.
 */

#ifndef TOYENGINE_RIG_IK_COMPONENTS_H
#define TOYENGINE_RIG_IK_COMPONENTS_H

#include <toyengine/anim/ik.h>
#include <toyengine/anim/ik_components.h> // resolve_ik_path()
#include <toyengine/rig/rig_component.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace toy {
namespace rig {

/**
 * @class IkComponent
 * @brief Common base of every component IkSystem runs, so its per-frame scene walk costs one
 *        dynamic_cast per component — mirrors toy::anim::IkComponent exactly.
 */
class IkComponent : public coopa::scene::Component {
public:
    enum class Kind { Chain, TwoBone, LookAt };
    virtual Kind ik_kind() const = 0;

    /// Order: lower first. Solvers -- ties: chains, then two-bone, then look-at.
    int priority = 0;

    /** @brief Optional explicit rig (a scene path); empty = the nearest ancestor carrying a Rig. */
    std::string rig_path;

    virtual void solve(float dt) = 0;

    /** @brief The rig this solver poses, or null if none was found. Bound at start()/bind(). */
    Rig* rig() const { return _rig; }

protected:
    Rig* resolve_rig_() const {
        if (!rig_path.empty() && scene) {
            if (auto* o = scene->find_object_by_path(rig_path)) {
                if (auto* r = o->get_component<Rig>()) return r;
            }
        }
        for (coopa::scene::SceneObject* o = owner; o; o = o->parent()) {
            if (auto* r = o->get_component<Rig>()) return r;
        }
        return nullptr;
    }

    /** @brief Bone i's world position (its resolved head, this frame). */
    glm::vec3 bone_world_position_(int i) const {
        return _rig && i >= 0 ? glm::vec3(_rig->world_matrix(static_cast<size_t>(i))[3]) : glm::vec3(0.0f);
    }

    /** @brief Bone i's world rotation (this frame). */
    glm::quat bone_world_rotation_(int i) const {
        return _rig && i >= 0 ? toy::anim::ik::rotation_of(_rig->world_matrix(static_cast<size_t>(i))) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    /** @brief The world rotation bone i's pose composes onto -- its parent bone's world rotation, or the rig owner's if i is a root bone. */
    glm::quat bone_parent_world_rotation_(int i) const {
        if (!_rig || i < 0) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const int p = _rig->bone_parent(static_cast<size_t>(i));
        if (p >= 0) return bone_world_rotation_(p);
        auto* tc = _rig->owner ? _rig->owner->get_transform() : nullptr;
        return tc ? toy::anim::ik::rotation_of(tc->get_world_matrix()) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    }

    /**
     * @brief Pre-multiplies `world_delta` (blended toward identity by `weight`) onto bone i's
     *        current world rotation, and stores the result back as bone i's pose rotation.
     *
     * Converts through bone i's rest-local rotation so the stored value is a PURE POSE DELTA
     * (Rig::PoseChannel's contract) rather than bone i's full local rotation -- see
     * toy::anim::ik::to_local_delta's doc for the parent/local/delta algebra this performs.
     */
    void apply_world_delta_(int i, const glm::quat& world_delta, float weight) {
        if (!_rig || i < 0) return;
        const glm::quat blended = (weight >= 0.9999f) ? world_delta : glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), world_delta, weight);
        const glm::quat parent_world_rot = bone_parent_world_rotation_(i);
        const glm::quat rest_local_rot = toy::anim::ik::rotation_of(_rig->rest_local_matrix(static_cast<size_t>(i)));
        const glm::quat cur_local_full = rest_local_rot * _rig->pose(static_cast<size_t>(i)).rotation;
        const glm::quat new_local_full = toy::anim::ik::to_local_delta(parent_world_rot, cur_local_full, blended);
        _rig->pose(static_cast<size_t>(i)).rotation = glm::normalize(glm::inverse(rest_local_rot) * new_local_full);
    }

    Rig* _rig = nullptr;
};

/**
 * @class TwoBoneIK
 * @brief Bends an upper/lower bone pair (thigh/shin, upper/lower arm) so the end bone's head
 *        reaches a target, the middle joint bending toward an optional pole. See
 *        toy::anim::TwoBoneIK's doc -- identical contract, bone-space.
 */
class TwoBoneIK : public IkComponent {
public:
    std::string type_name() const override { return "TwoBoneIK"; }
    Kind ik_kind() const override { return Kind::TwoBone; }

    std::string upper, lower, end; ///< Bone names in the rig.
    std::string target;            ///< Target object path.
    std::string pole;               ///< Optional pole object path.
    float weight = 1.0f;
    float soft_limit = 0.02f;
    bool match_target_rotation = false;

    void start() override { bind(); }
    void rebind() { _bound = false; }
    void bind() { if (!_bound) bind_(); }

    const toy::anim::ik::TwoBoneResult& last_result() const { return _result; }

    /** @brief The authored target object (never the override below), or null. For a driver
     *         (FootIK, Rider) that needs to read where the clip itself put the target before
     *         overriding it. */
    coopa::scene::SceneObject* target_object() const { return _target_obj; }
    /** @brief Bone i's world position, for a driver that needs e.g. the hip to place a pole. */
    glm::vec3 upper_world_position() const { return bone_world_position_(_upper); }
    glm::vec3 lower_world_position() const { return bone_world_position_(_lower); }
    glm::vec3 end_world_position() const { return bone_world_position_(_end); }

    /**
     * @brief Overrides this frame's target/pole position, in place of resolving `target`/`pole`
     *        from the scene -- a driver (FootIK's targets mode, Rider) steers the SAME solver
     *        the clip authored rather than adding a second one. Cleared by clear_target_override().
     */
    void set_target_position(const glm::vec3& world_pos) { _target_override = world_pos; }
    void set_pole_position(const glm::vec3& world_pos) { _pole_override = world_pos; }
    void clear_target_override() { _target_override.reset(); _pole_override.reset(); }

    void solve(float /*dt*/) override {
        if (!_bound) bind_();
        if (!_rig || _upper < 0 || _lower < 0 || _end < 0) return;
        const float w = std::clamp(weight, 0.0f, 1.0f);
        if (w <= 0.0f) return;
        if (!_target_override && !_target_obj) return;
        const glm::vec3 tgt = _target_override ? *_target_override : glm::vec3(_target_obj->get_transform()->get_world_matrix()[3]);

        const glm::vec3 a = bone_world_position_(_upper);
        const glm::vec3 b = bone_world_position_(_lower);
        const glm::vec3 c = bone_world_position_(_end);
        const bool has_pole = _pole_override.has_value() || _pole_obj != nullptr;
        const glm::vec3 pole_p = _pole_override ? *_pole_override
                                : (_pole_obj ? glm::vec3(_pole_obj->get_transform()->get_world_matrix()[3]) : glm::vec3(0.0f));
        _result = toy::anim::ik::solve_two_bone(a, b, c, tgt, pole_p, has_pole, soft_limit);

        apply_world_delta_(_upper, _result.upper_delta, w);
        // The lower bone's parent chain just changed: re-solve its swing from the new pose, so a
        // partial weight still leaves the two bones consistent.
        const glm::vec3 b2 = bone_world_position_(_lower);
        const glm::vec3 c2 = bone_world_position_(_end);
        const glm::vec3 want_end = glm::mix(c, _result.end, w);
        glm::quat lo_delta(1.0f, 0.0f, 0.0f, 0.0f);
        if (glm::length(c2 - b2) > 1e-6f && glm::length(want_end - b2) > 1e-6f) {
            lo_delta = toy::anim::ik::rotation_between(glm::normalize(c2 - b2), glm::normalize(want_end - b2));
        }
        apply_world_delta_(_lower, lo_delta, w);

        // An override carries no rotation of its own (a driver steers position only), so this
        // only runs against the authored target object -- same as before overrides existed.
        if (match_target_rotation && !_target_override && _target_obj) {
            const glm::quat want = toy::anim::ik::rotation_of(_target_obj->get_transform()->get_world_matrix());
            const glm::quat have = bone_world_rotation_(_end);
            const glm::quat delta = glm::normalize(want * glm::inverse(have));
            apply_world_delta_(_end, delta, w);
        }
    }

private:
    /**
     * @brief Resolves the rig, bones and target/pole objects. NOT marked bound unless the
     * bones actually resolved: a Rig's RigAsset loads asynchronously (coopa/asset/
     * asset_manager.h), and this component's own start() -- the very next component after
     * "Rig" on the same object, in the same call, with no yield point between them -- can run
     * before that load finishes, leaving find_bone() looking at an empty asset. Retrying every
     * solve() call until the bones resolve (matching FootIK/BoneAttachment's own self-healing
     * pattern) makes this a one-frame hiccup instead of a permanently broken IK driver.
     */
    void bind_() {
        _rig = resolve_rig_();
        _upper = _rig ? _rig->find_bone(upper) : -1;
        _lower = _rig ? _rig->find_bone(lower) : -1;
        _end = _rig ? _rig->find_bone(end) : -1;
        _target_obj = target.empty() ? nullptr : toy::anim::resolve_ik_path(owner, scene, target);
        _pole_obj = pole.empty() ? nullptr : toy::anim::resolve_ik_path(owner, scene, pole);
        _bound = _rig && _upper >= 0 && _lower >= 0 && _end >= 0;
    }

    bool _bound = false;
    int _upper = -1, _lower = -1, _end = -1;
    coopa::scene::SceneObject* _target_obj = nullptr;
    coopa::scene::SceneObject* _pole_obj = nullptr;
    std::optional<glm::vec3> _target_override, _pole_override;
    toy::anim::ik::TwoBoneResult _result;
};

/**
 * @class ChainIK
 * @brief Bends a chain of bones of any length -- a spine, a neck, a tail -- so the tip bone's
 *        head reaches a target (FABRIK). See toy::anim::ChainIK's doc -- identical
 *        contract, bone-space: the chain is `root` and its descendants down to `tip`,
 *        WITHIN THE RIG's bone hierarchy (RigAsset::bones' parent links), not the scene graph.
 */
class ChainIK : public IkComponent {
public:
    std::string type_name() const override { return "ChainIK"; }
    Kind ik_kind() const override { return Kind::Chain; }

    std::string root, tip;  ///< Bone names: root pivots in place; tip's head reaches the target.
    std::string target;
    float weight = 1.0f;
    int iterations = 10;
    float tolerance = 1e-3f;
    bool match_target_rotation = false;

    void start() override { bind(); }
    void rebind() { _bound = false; }
    void bind() { if (!_bound) bind_(); }

    const std::vector<int>& bones() const { return _chain; }
    bool reached() const { return _reached; }

    /** @brief The authored target object (never the override below), or null. */
    coopa::scene::SceneObject* target_object() const { return _target_obj; }
    /** @brief Overrides this frame's target position -- see TwoBoneIK's identical method. */
    void set_target_position(const glm::vec3& world_pos) { _target_override = world_pos; }
    void clear_target_override() { _target_override.reset(); }

    void solve(float /*dt*/) override {
        if (!_bound) bind_();
        if (_chain.size() < 2) return;
        const float w = std::clamp(weight, 0.0f, 1.0f);
        if (w <= 0.0f || (!_target_override && !_target_obj)) return;
        const glm::vec3 tgt = _target_override ? *_target_override : glm::vec3(_target_obj->get_transform()->get_world_matrix()[3]);

        std::vector<glm::vec3> joints(_chain.size());
        for (size_t i = 0; i < _chain.size(); ++i) joints[i] = bone_world_position_(_chain[i]);
        std::vector<glm::vec3> solved = joints;
        _reached = toy::anim::ik::solve_chain(solved, tgt, iterations, tolerance);
        for (size_t i = 0; i < _chain.size(); ++i) solved[i] = glm::mix(joints[i], solved[i], w);

        for (size_t i = 0; i + 1 < _chain.size(); ++i) {
            const glm::vec3 a = bone_world_position_(_chain[i]);
            const glm::vec3 b = bone_world_position_(_chain[i + 1]);
            glm::quat delta(1.0f, 0.0f, 0.0f, 0.0f);
            if (glm::length(b - a) > 1e-6f && glm::length(solved[i + 1] - a) > 1e-6f) {
                delta = toy::anim::ik::rotation_between(glm::normalize(b - a), glm::normalize(solved[i + 1] - a));
            }
            apply_world_delta_(_chain[i], delta, 1.0f); // already blended into `solved` by weight
        }
        if (match_target_rotation && !_target_override && _target_obj) {
            const glm::quat want = toy::anim::ik::rotation_of(_target_obj->get_transform()->get_world_matrix());
            const glm::quat have = bone_world_rotation_(_chain.back());
            const glm::quat delta = glm::normalize(want * glm::inverse(have));
            apply_world_delta_(_chain.back(), delta, w);
        }
    }

private:
    /** @brief See TwoBoneIK::bind_()'s doc for why this retries (not marked bound) until the
     *         chain actually resolves -- the Rig's async asset load can still be pending the
     *         first time this runs. */
    void bind_() {
        _rig = resolve_rig_();
        _chain.clear();
        if (_rig) {
            const int r = _rig->find_bone(root);
            const int t = _rig->find_bone(tip);
            for (int b = t; b >= 0; b = _rig->bone_parent(static_cast<size_t>(b))) {
                _chain.push_back(b);
                if (b == r) break;
            }
            if (r < 0 || _chain.empty() || _chain.back() != r) _chain.clear();
            std::reverse(_chain.begin(), _chain.end());
        }
        _target_obj = target.empty() ? nullptr : toy::anim::resolve_ik_path(owner, scene, target);
        _bound = _rig && _chain.size() >= 2;
    }

    bool _bound = false;
    std::vector<int> _chain;
    coopa::scene::SceneObject* _target_obj = nullptr;
    std::optional<glm::vec3> _target_override;
    bool _reached = false;
};

/**
 * @class LookAtIK
 * @brief Turns one bone (a head, an eye, a turret) so its forward axis faces a target, at most
 *        `max_angle` from its animated facing, with optional exponential smoothing. See
 *        toy::anim::LookAtIK's doc -- identical contract, bone-space.
 */
class LookAtIK : public IkComponent {
public:
    std::string type_name() const override { return "LookAtIK"; }
    Kind ik_kind() const override { return Kind::LookAt; }

    std::string bone; ///< Bone name; empty = the rig's root bone (index 0).
    std::string target;
    glm::vec3 forward_axis{0.0f, 1.0f, 0.0f};
    glm::vec3 up_axis{0.0f, 0.0f, 1.0f};
    float max_angle = 70.0f;
    float weight = 1.0f;
    float smoothing = 0.0f;

    void start() override { bind_(); }
    void rebind() { _bound = false; }

    const glm::quat& applied_delta() const { return _smoothed; }

    void solve(float dt) override {
        if (!_bound) bind_();
        if (!_rig || _bone < 0) return;
        const float w = std::clamp(weight, 0.0f, 1.0f);
        glm::quat desired(1.0f, 0.0f, 0.0f, 0.0f);
        if (_target_obj && w > 0.0f && glm::dot(forward_axis, forward_axis) > 1e-10f) {
            const glm::vec3 pos = bone_world_position_(_bone);
            const glm::quat wr = bone_world_rotation_(_bone);
            const glm::vec3 tgt = glm::vec3(_target_obj->get_transform()->get_world_matrix()[3]);
            const glm::vec3 to = tgt - pos;
            if (glm::dot(to, to) > 1e-10f) {
                const glm::vec3 f = glm::normalize(wr * glm::normalize(forward_axis));
                glm::vec3 u = glm::dot(up_axis, up_axis) > 1e-10f ? glm::normalize(wr * glm::normalize(up_axis)) : glm::vec3(0.0f, 0.0f, 1.0f);
                desired = toy::anim::ik::look_at_delta(f, u, glm::normalize(to), glm::radians(std::max(0.0f, max_angle)));
                desired = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), desired, w);
            }
        }
        if (smoothing > 0.0f && _has_smoothed) {
            const float k = 1.0f - std::exp(-std::max(dt, 0.0f) / smoothing);
            _smoothed = glm::normalize(glm::slerp(_smoothed, desired, k));
        } else {
            _smoothed = desired;
        }
        _has_smoothed = true;
        apply_world_delta_(_bone, _smoothed, 1.0f); // already weighted/smoothed above
    }

private:
    /** @brief See TwoBoneIK::bind_()'s doc for why this retries (not marked bound) until the
     *         bone actually resolves -- the Rig's async asset load can still be pending the
     *         first time this runs. */
    void bind_() {
        _rig = resolve_rig_();
        _bone = _rig ? (bone.empty() ? 0 : _rig->find_bone(bone)) : -1;
        _target_obj = target.empty() ? nullptr : toy::anim::resolve_ik_path(owner, scene, target);
        _smoothed = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        _has_smoothed = false;
        _bound = _rig && _bone >= 0;
    }

    bool _bound = false;
    int _bone = -1;
    coopa::scene::SceneObject* _target_obj = nullptr;
    glm::quat _smoothed{1.0f, 0.0f, 0.0f, 0.0f};
    bool _has_smoothed = false;
};

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_IK_COMPONENTS_H
