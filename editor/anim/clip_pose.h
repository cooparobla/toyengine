/**
 * @file clip_pose.h
 * @brief Poses a live rig with a clip at a time -- the Timeline's preview in edit mode, where the
 *        scene does not simulate (coopa's AnimationSystem only runs while it does).
 *
 * Resolution and writes match coopa::anim::Animator's: a track's object path is resolved from
 * the rig root ("" the root, "A/B" walking down, a bare name at any depth), its property through
 * coopa::anim::AnimatedPropertyRegistry (channel suffixes like position.x included), sampled
 * with AnimationCurve::sample(). Procedural tracks are skipped: they need the runtime's inputs.
 * A clip's root-motion channels (AnimationClip::root_motion) are held at their clip-start value,
 * as the Animator holds them, so a walk previews in place.
 */

#ifndef TOYEDITOR_ANIM_CLIP_POSE_H
#define TOYEDITOR_ANIM_CLIP_POSE_H

#include <coopa/animation/animated_property.h>
#include <coopa/animation/animation_clip.h>
#include <coopa/scene/scene_object.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace toy::editor {

/** @brief The object `path` names under `root` ("" = root), or null -- coopa::anim::Animator's
 *         rule exactly: every segment is found among the descendants of the previous one. */
coopa::scene::SceneObject* resolve_rig_path(coopa::scene::SceneObject* root, const std::string& path);

/** @brief `t` folded into [0, length] per the clip's wrap mode (as the Animator plays it). */
float wrap_clip_time(const coopa::anim::AnimationClip& clip, float t);

/**
 * @brief Writes the clip's pose at clip time `t` (not wrapped here) onto the objects under `root`.
 * @return How many tracks were applied.
 */
int apply_clip_pose(const coopa::anim::AnimationClip& clip, float t, coopa::scene::SceneObject* root);

}  // namespace toy::editor

#endif  // TOYEDITOR_ANIM_CLIP_POSE_H
