/**
 * @file rig_pose_math.h
 * @brief Free-function versions of ik_components.h's bone world-space <-> pose-delta math,
 *        usable by a driver that does NOT inherit coopa::rig::IkComponent (a toyengine
 *        system like FootIK or Ragdoll, which needs physics/raycasts an IkComponent has no
 *        business depending on). Identical algebra -- see ik_components.h's IkComponent for
 *        the derivation notes; kept here so neither side duplicates it by copy-paste drift.
 */

#ifndef COOPA_RIG_RIG_POSE_MATH_H
#define COOPA_RIG_RIG_POSE_MATH_H

#include <coopa/animation/ik.h> // rotation_of(), to_local_delta()
#include <coopa/rig/rig_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace coopa {
namespace rig {

/** @brief Bone i's world position (its resolved head, this frame). */
inline glm::vec3 bone_world_position(const Rig& rig, int i) {
    return i >= 0 ? glm::vec3(rig.world_matrix(static_cast<size_t>(i))[3]) : glm::vec3(0.0f);
}

/** @brief Bone i's world rotation (this frame). */
inline glm::quat bone_world_rotation(const Rig& rig, int i) {
    return i >= 0 ? coopa::anim::ik::rotation_of(rig.world_matrix(static_cast<size_t>(i))) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

/** @brief The world rotation bone i's pose composes onto -- its parent bone's, or the rig owner's if i is a root bone. */
inline glm::quat bone_parent_world_rotation(const Rig& rig, int i) {
    if (i < 0) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const int p = rig.bone_parent(static_cast<size_t>(i));
    if (p >= 0) return bone_world_rotation(rig, p);
    auto* tc = rig.owner ? rig.owner->get_transform() : nullptr;
    return tc ? coopa::anim::ik::rotation_of(tc->get_world_matrix()) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
}

/**
 * @brief Pre-multiplies `world_delta` (blended toward identity by `weight`) onto bone i's
 *        current world rotation, and stores the result back as bone i's pose rotation. See
 *        ik_components.h's IkComponent::apply_world_delta_ for the parent/local/delta algebra.
 */
inline void apply_world_rotation_delta(Rig& rig, int i, const glm::quat& world_delta, float weight) {
    if (i < 0) return;
    const glm::quat blended = (weight >= 0.9999f) ? world_delta : glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), world_delta, weight);
    const glm::quat parent_world_rot = bone_parent_world_rotation(rig, i);
    const glm::quat rest_local_rot = coopa::anim::ik::rotation_of(rig.rest_local_matrix(static_cast<size_t>(i)));
    const glm::quat cur_local_full = rest_local_rot * rig.pose(static_cast<size_t>(i)).rotation;
    const glm::quat new_local_full = coopa::anim::ik::to_local_delta(parent_world_rot, cur_local_full, blended);
    rig.pose(static_cast<size_t>(i)).rotation = glm::normalize(glm::inverse(rest_local_rot) * new_local_full);
}

/**
 * @brief Adds a WORLD-space translation delta to bone i's resolved head, leaving its
 *        rotation untouched -- the translation counterpart to apply_world_rotation_delta,
 *        for a driver (FootIK's pelvis drop) that needs to shift a bone's position rather
 *        than its orientation. Converts the world delta into bone i's own pose-local frame:
 *        pose.location += inverse(parent_bone_world_rotation * rest_local_rotation) *
 *        world_delta -- bone_parent_world_rotation() already folds in the rig owner's own
 *        world rotation (it bottoms out at the owner's Transform for a root bone), so it must
 *        NOT be removed a second time here.
 */
inline void apply_world_translation_delta(Rig& rig, int i, const glm::vec3& world_delta) {
    if (i < 0) return;
    const glm::quat parent_world_rot = bone_parent_world_rotation(rig, i);
    const glm::quat rest_local_rot = coopa::anim::ik::rotation_of(rig.rest_local_matrix(static_cast<size_t>(i)));
    const glm::quat frame_rot = parent_world_rot * rest_local_rot;
    rig.pose(static_cast<size_t>(i)).location += glm::inverse(frame_rot) * world_delta;
}

} // namespace rig
} // namespace coopa

#endif // COOPA_RIG_RIG_POSE_MATH_H
