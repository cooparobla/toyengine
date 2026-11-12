/**
 * @file rig_component.h
 * @brief Rig component: a RigAsset reference plus the live pose it is driven to.
 */

#ifndef TOYENGINE_RIG_RIG_COMPONENT_H
#define TOYENGINE_RIG_RIG_COMPONENT_H

#include <coopa/asset/asset_handle.h>
#include <toyengine/rig/rig_asset.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace toy {
namespace rig {

class RigPoseSystem; // Forward declaration — Rig grants it friend access; see rig_system.h.

/** @brief Composes a translate * rotate * scale matrix (bone-local TRS). */
inline glm::mat4 compose_trs(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r);
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    return m;
}

/**
 * @class Rig
 * @brief A skeleton: a RigAsset reference, plus a per-bone pose delta and the model
 *        matrices resolved from it.
 *
 * Deliberately carries no Component::update()/late_update() override — exactly like
 * toy::anim::Animator, a Rig never resolves its own pose on a fixed schedule. Resolution
 * (pose deltas -> model matrices) happens lazily, on demand, mirroring
 * coopa::util::Transform's own dirty-flag pattern exactly: writing a pose channel
 * (pose()/clear_pose()) marks the Rig dirty, and the next read of model_matrix()/
 * world_matrix() resolves it first. This is what lets rig-space IK (toyengine/rig/ik_system.h,
 * order 320) read bone WORLD positions reflecting this frame's animated pose (written by a
 * SkeletalAnimator at order 300), then write new pose deltas of its own, and have a LATER
 * reader (skinning, at upload time) see the fully-resolved result — a single eager pass
 * (RigPoseSystem, order 340) could not serve both "IK's input must be post-animation" and
 * "IK's output must be resolved before it is read" within the one frame. RigPoseSystem is
 * kept anyway as a deterministic end-of-phase pass: resolve_() is idempotent (a pure
 * function of the current pose), so forcing it again there is always safe, and it guarantees
 * a Rig nothing else reads this frame is still resolved for the systems after it.
 *
 * A pose channel is a delta from rest, expressed in the bone's own REST-LOCAL frame
 * (Blender's pose-channel semantics): identity means "exactly the rest pose". This is what
 * makes clear_pose() trivial and a clip retargetable onto any rig sharing its bone names.
 */
class Rig : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Rig"; }

    /** @brief The rig asset's YAML path, as written in the scene (for inspection/round-trip). */
    std::string rig_path;
    /** @brief Set by the "Rig" component parser (rig_yaml.h) from rig_path. */
    coopa::asset::AssetHandle<RigAsset> asset;

    void start() override { ensure_bound_(); }

    // --- Bone lookup (valid once asset is loaded; otherwise return empty/invalid values) ---

    bool is_loaded() const { return asset.is_loaded(); }
    size_t bone_count() const { return asset.is_loaded() ? asset.get()->bones.size() : 0; }
    int find_bone(const std::string& name) const { return asset.is_loaded() ? asset.get()->find_bone(name) : -1; }

    const std::string& bone_name(size_t i) const {
        static const std::string empty;
        return asset.is_loaded() ? asset.get()->bones[i].name : empty;
    }
    int bone_parent(size_t i) const { return asset.is_loaded() ? asset.get()->bones[i].parent : -1; }
    float bone_length(size_t i) const { return asset.is_loaded() ? asset.get()->bone_length(i) : 0.0f; }

    /** @brief Bone i's rest head, in rig space. */
    glm::vec3 bone_head_rest(size_t i) const {
        return asset.is_loaded() ? glm::vec3(asset.get()->rest_model()[i][3]) : glm::vec3(0.0f);
    }
    /** @brief Bone i's rest tail, in rig space. */
    glm::vec3 bone_tail_rest(size_t i) const {
        return asset.is_loaded() ? asset.get()->bones[i].tail : glm::vec3(0.0f);
    }

    // --- Pose: a delta from rest, in the bone's own rest-local frame ---

    /** @struct PoseChannel @brief One bone's pose delta. Identity = exactly the rest pose. */
    struct PoseChannel {
        glm::vec3 location{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f};
    };

    /** @brief Mutable access to bone i's pose channel. Self-heals via ensure_bound_() so a
     *         writer (a SkeletalAnimator, an IK solver) never races Rig's own start()/resolve_()
     *         for which one sizes the pose buffer first. */
    PoseChannel& pose(size_t i) {
        ensure_bound_();
        _dirty = true;
        return _pose[i];
    }
    const PoseChannel& pose(size_t i) const { return _pose[i]; }

    /** @brief Resets every bone's pose channel to identity (the rest pose). */
    void clear_pose() {
        ensure_bound_();
        _pose.assign(_pose.size(), PoseChannel{});
        _dirty = true;
    }

    // --- Resolved state: lazily kept current with the pose (see this class's doc) ---

    /** @brief Bone i's resolved matrix this frame, bone space -> rig space (this object's own Transform). */
    const glm::mat4& model_matrix(size_t i) const {
        resolve_if_dirty_();
        return _model[i];
    }

    /** @brief Bone i's resolved matrix this frame, bone space -> world space. */
    glm::mat4 world_matrix(size_t i) const {
        resolve_if_dirty_();
        auto* tc = owner ? owner->get_transform() : nullptr;
        return (tc ? tc->get_world_matrix() : glm::mat4(1.0f)) * _model[i];
    }

    /** @brief Bone i's REST matrix, bone space -> rig space (ignores the current pose). */
    glm::mat4 rest_model_matrix(size_t i) const { return asset.is_loaded() ? asset.get()->rest_model()[i] : glm::mat4(1.0f); }

    /** @brief Bone i's REST matrix in its parent's bone space (root: same as rest_model_matrix(i)). */
    glm::mat4 rest_local_matrix(size_t i) const { return asset.is_loaded() ? asset.get()->rest_local()[i] : glm::mat4(1.0f); }

    /** @brief The inverse bind matrix a skinning palette needs: inverse(rest_model_matrix(i)). */
    glm::mat4 inverse_bind(size_t i) const { return glm::inverse(rest_model_matrix(i)); }

    /** @brief Bumped every time resolve_() runs — lets a dependent (skinning, a drawer) detect a new pose cheaply. */
    uint32_t pose_revision() const { return _pose_revision; }

private:
    friend class RigPoseSystem;

    /**
     * @brief (Re)sizes _pose/_model to the asset's bone count the first time it loads, and
     *        again whenever its AssetHandle::revision() moves (a hot reload) — mirroring
     *        toy::anim::Animator::ensure_state_bound_()'s lazy-rebind trick. A reload resets
     *        every pose channel to identity: the old bone indices may no longer mean the
     *        same joints.
     */
    void ensure_bound_() {
        if (!asset.is_valid() || !asset.is_loaded()) return;
        if (_bound && _bound_revision == asset.revision()) return;
        const size_t n = asset.get()->bones.size();
        _pose.assign(n, PoseChannel{});
        _model.assign(n, glm::mat4(1.0f));
        _bound_revision = asset.revision();
        _bound = true;
        _dirty = true;
    }

    /** @brief Resolves now if a pose write happened since the last resolve. Main thread only. */
    void resolve_if_dirty_() const {
        if (!_dirty) return;
        const_cast<Rig*>(this)->resolve_();
    }

    /** @brief Recomputes every bone's model matrix from its rest pose and current pose delta. Main thread only. */
    void resolve_() {
        ensure_bound_();
        if (!asset.is_loaded()) { _dirty = false; return; }
        const RigAsset& ra = *asset.get();
        for (size_t i = 0; i < ra.bones.size(); ++i) {
            const glm::mat4 pose_local = ra.rest_local()[i] * compose_trs(_pose[i].location, _pose[i].rotation, _pose[i].scale);
            const int parent = ra.bones[i].parent;
            _model[i] = parent >= 0 ? _model[static_cast<size_t>(parent)] * pose_local : pose_local;
        }
        ++_pose_revision;
        _dirty = false;
    }

    bool     _bound = false;
    bool     _dirty = true; // starts dirty so the first read resolves even an untouched (rest) pose
    uint32_t _bound_revision = 0;
    uint32_t _pose_revision = 0;
    std::vector<PoseChannel> _pose;
    std::vector<glm::mat4>   _model;
};

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_RIG_COMPONENT_H
