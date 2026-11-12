/**
 * @file clip_pose.h
 * @brief Poses a live rig with a clip at a time -- the Timeline's preview in edit mode, where the
 *        scene does not simulate (coopa's AnimationSystem only runs while it does).
 *
 * Resolution and writes match toy::anim::Animator's: a track's object path is resolved from
 * the rig root ("" the root, "A/B" walking down, a bare name at any depth), its property through
 * toy::anim::AnimatedPropertyRegistry (channel suffixes like position.x included), sampled
 * with AnimationCurve::sample(). Procedural tracks are skipped: they need the runtime's inputs.
 * A clip's root-motion channels (AnimationClip::root_motion) are held at their clip-start value,
 * as the Animator holds them, so a walk previews in place.
 */

#ifndef TOYEDITOR_ANIM_CLIP_POSE_H
#define TOYEDITOR_ANIM_CLIP_POSE_H

#include <toyengine/anim/animated_property.h>
#include <toyengine/anim/animation_clip.h>
#include <toyengine/rig/rig_component.h>
#include <toyengine/rig/skeletal_clip.h>
#include <coopa/scene/scene_object.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace toy::editor {

/** @brief The object `path` names under `root` ("" = root), or null -- toy::anim::Animator's
 *         rule exactly: every segment is found among the descendants of the previous one. */
coopa::scene::SceneObject* resolve_rig_path(coopa::scene::SceneObject* root, const std::string& path);

/** @brief `t` folded into [0, length] per the clip's wrap mode (as the Animator plays it). */
float wrap_clip_time(const toy::anim::AnimationClip& clip, float t);

/**
 * @brief Writes the clip's pose at clip time `t` (not wrapped here) onto the objects under `root`.
 * @return How many tracks were applied.
 */
int apply_clip_pose(const toy::anim::AnimationClip& clip, float t, coopa::scene::SceneObject* root);

/**
 * @brief Writes a SkeletalClip's pose at clip time `t` (not wrapped here) into `rig`'s pose
 *        buffer -- the bone-addressed sibling of apply_clip_pose(), for the Timeline's edit-mode
 *        preview over a toy::rig::Rig character. Bone lookup is by name
 *        (`toy::rig::Rig::find_bone`), matching `toy::rig::SkeletalAnimator::rebind()`'s own
 *        rule. A channel with no keys is left untouched (a hand-posed or IK-driven bone shows
 *        through). Root-motion translation/yaw is held at the clip's start value, exactly as
 *        `toy::rig::SkeletalAnimator::write_sampled_` holds it during real playback, so a
 *        walk cycle previews in place rather than sliding the rig out of the viewport.
 * @return How many bone tracks were applied.
 */
int apply_bone_pose(const toy::rig::SkeletalClip& clip, float t, toy::rig::Rig* rig);

}  // namespace toy::editor

#endif  // TOYEDITOR_ANIM_CLIP_POSE_H
