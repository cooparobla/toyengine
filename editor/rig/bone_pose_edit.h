/**
 * @file bone_pose_edit.h
 * @brief Blender-style pose-mode edits on a coopa::rig::Rig's pose buffer: world-space G/R/S
 *        deltas converted into the bone's own pose-delta frame, Clear ops, mirror, copy/paste.
 *
 * Pure over `coopa::rig::Rig` -- no viewport, no input, no undo (the panel wraps these calls
 * with its own `UndoStack<RigPoseSnapshot>`-style command, same pattern as every other
 * `editor/<module>/` function). `editor/viewport/modal_transform.h`'s `ModalTransform` already
 * computes a world-space delta (translate / rotate axis+angle / scale) independent of what is
 * being transformed; this file is the other half -- what a bone's pose channel does with that
 * delta.
 *
 * The rotation conversion reuses `coopa::anim::ik::to_local_delta` verbatim: it is the exact
 * same world-delta-to-local-delta algebra `coopa::rig::ik_components.h`'s `TwoBoneIK` etc.
 * already apply every frame, not a reimplementation. The translate conversion follows the same
 * reasoning (see `world_to_pose_frame`'s doc) -- there is no existing precedent for it, since no
 * IK solver ever moves a bone's origin, only rotates it.
 */

#ifndef TOYENGINE_EDITOR_RIG_BONE_POSE_EDIT_H
#define TOYENGINE_EDITOR_RIG_BONE_POSE_EDIT_H

#include <coopa/animation/ik.h>
#include <coopa/rig/rig_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace toy {
namespace editor {
namespace rig_pose_edit {

/** @brief Bone `i`'s parent's world rotation -- the rig's own owner's world rotation for a root bone. */
inline glm::quat parent_world_rotation(const coopa::rig::Rig& rig, std::size_t i) {
    const int p = rig.bone_parent(i);
    if (p >= 0) return coopa::anim::ik::rotation_of(rig.world_matrix(static_cast<std::size_t>(p)));
    auto* tc = rig.owner ? rig.owner->get_transform() : nullptr;
    return tc ? coopa::anim::ik::rotation_of(tc->get_world_matrix()) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

/**
 * @brief The rotation of the frame `pose(i).location` is expressed in: the parent's world
 *        rotation composed with bone `i`'s own rest-local rotation.
 *
 * `Rig::resolve_()` computes `pose_local = rest_local[i] * compose_trs(loc, rot, scale)`, and
 * `compose_trs`'s translation column is exactly `loc` regardless of `rot`/`scale` (rotation and
 * per-axis scale only touch the matrix's linear columns). So bone `i`'s world HEAD is
 * `model[parent] * rest_local[i]` applied to the point `loc` -- an affine map whose rotation
 * part is this frame. A small `loc` delta therefore moves the world head by this frame's
 * rotation applied to that delta, which is what `apply_translate_delta` inverts.
 */
inline glm::quat world_to_pose_frame(const coopa::rig::Rig& rig, std::size_t i) {
    return parent_world_rotation(rig, i) * coopa::anim::ik::rotation_of(rig.rest_local_matrix(i));
}

/** @brief Applies a WORLD-space translation delta to bone `i`'s pose location (Grab). */
inline void apply_translate_delta(coopa::rig::Rig& rig, std::size_t i, const glm::vec3& world_translate) {
    const glm::quat frame = world_to_pose_frame(rig, i);
    rig.pose(i).location += glm::inverse(frame) * world_translate;
}

/**
 * @brief Applies a WORLD-space rotation delta (axis, degrees) to bone `i`'s pose rotation
 *        (Rotate) -- the same conversion `coopa::rig::IkComponent::apply_world_delta_` uses.
 */
inline void apply_rotate_delta(coopa::rig::Rig& rig, std::size_t i, const glm::vec3& world_axis, float angle_deg) {
    if (std::abs(angle_deg) < 1e-6f) return;
    const glm::quat world_delta = glm::angleAxis(glm::radians(angle_deg), glm::normalize(world_axis));
    const glm::quat parent_world_rot = parent_world_rotation(rig, i);
    const glm::quat rest_local_rot = coopa::anim::ik::rotation_of(rig.rest_local_matrix(i));
    const glm::quat cur_local_full = rest_local_rot * rig.pose(i).rotation;
    const glm::quat new_local_full = coopa::anim::ik::to_local_delta(parent_world_rot, cur_local_full, world_delta);
    rig.pose(i).rotation = glm::normalize(glm::inverse(rest_local_rot) * new_local_full);
}

/**
 * @brief Applies a rotation delta about bone `i`'s OWN current axis (local +Y) -- Blender's
 *        "R, R" twist, pressed a second time while rotating.
 */
inline void apply_twist_delta(coopa::rig::Rig& rig, std::size_t i, float angle_deg) {
    const glm::vec3 bone_y = glm::normalize(glm::vec3(rig.world_matrix(i)[1]));
    apply_rotate_delta(rig, i, bone_y, angle_deg);
}

/** @brief Multiplies bone `i`'s pose scale by a LOCAL factor (Scale) -- scale has no sensible
 *         "world" reading once a parent chain of rotations is involved, so it is always local,
 *         matching Blender's own default (global-space pose-bone scale is rare and not modelled
 *         here). */
inline void apply_scale_delta(coopa::rig::Rig& rig, std::size_t i, const glm::vec3& local_factor) {
    rig.pose(i).scale *= local_factor;
}

/** @brief Resets bone `i`'s pose location to rest (0,0,0). */
inline void clear_location(coopa::rig::Rig& rig, std::size_t i) { rig.pose(i).location = glm::vec3(0.0f); }
/** @brief Resets bone `i`'s pose rotation to rest (identity). */
inline void clear_rotation(coopa::rig::Rig& rig, std::size_t i) { rig.pose(i).rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); }
/** @brief Resets bone `i`'s pose scale to rest (1,1,1). */
inline void clear_scale(coopa::rig::Rig& rig, std::size_t i) { rig.pose(i).scale = glm::vec3(1.0f); }

/**
 * @brief Copies bone `from`'s pose channel onto bone `to`, mirroring across the rig's local YZ
 *        plane (Blender's default pose mirror: a rig built with +X to one side, as every
 *        `_l`/`_r`-named rig in this codebase is -- see `tools/gen_mannequin.py`,
 *        `tools/gen_horse.py`). Location's X and the swing axes (Y, Z) of the rotation flip
 *        sign; the twist axis (bone-local Y, i.e. the rotation's own Y component after the
 *        location/rotation are both expressed bone-locally) does not, so a mirrored limb bends
 *        the same way instead of the opposite one.
 */
inline void mirror_pose(coopa::rig::Rig& rig, std::size_t from, std::size_t to) {
    const coopa::rig::Rig::PoseChannel src = rig.pose(from);
    coopa::rig::Rig::PoseChannel& dst = rig.pose(to);
    dst.location = glm::vec3(-src.location.x, src.location.y, src.location.z);
    dst.rotation = glm::quat(src.rotation.w, src.rotation.x, -src.rotation.y, -src.rotation.z);
    dst.scale = src.scale;
}

/** @brief A snapshot of a set of bones' pose channels, for copy/paste across a selection (or
 *         between two rigs sharing bone names -- see `coopa::rig::Rig`'s class doc on
 *         retargeting). Keyed by bone NAME, not index, so pasting onto a differently-ordered
 *         rig still lands on the right bones. */
struct PoseSnapshot {
    std::unordered_map<std::string, coopa::rig::Rig::PoseChannel> by_bone_name;
};

/** @brief Snapshots the pose channels of `bone_indices` on `rig`. */
inline PoseSnapshot copy_pose(const coopa::rig::Rig& rig, const std::vector<std::size_t>& bone_indices) {
    PoseSnapshot snap;
    for (std::size_t i : bone_indices) snap.by_bone_name[rig.bone_name(i)] = rig.pose(i);
    return snap;
}

/** @brief Pastes a snapshot onto `rig`, by bone name; a bone the snapshot has no entry for is
 *         left untouched. @return How many bones were actually pasted onto. */
inline std::size_t paste_pose(coopa::rig::Rig& rig, const PoseSnapshot& snap) {
    std::size_t applied = 0;
    for (std::size_t i = 0; i < rig.bone_count(); ++i) {
        auto it = snap.by_bone_name.find(rig.bone_name(i));
        if (it == snap.by_bone_name.end()) continue;
        rig.pose(i) = it->second;
        ++applied;
    }
    return applied;
}

} // namespace rig_pose_edit
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_RIG_BONE_POSE_EDIT_H
