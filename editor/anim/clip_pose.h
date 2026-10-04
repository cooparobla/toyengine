/**
 * @file clip_pose.h
 * @brief Poses a live rig with a clip at a time -- the Timeline's preview in edit mode, where the
 *        scene does not simulate (coopa's AnimationSystem only runs while it does).
 *
 * Resolution and writes match coopa::anim::Animator's: a track's object path is resolved from
 * the rig root ("" the root, "A/B" walking down, a bare name at any depth), its property through
 * coopa::anim::AnimatedPropertyRegistry (channel suffixes like position.x included), sampled
 * with AnimationCurve::sample(). Procedural tracks are skipped: they need the runtime's inputs.
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
inline coopa::scene::SceneObject* resolve_rig_path(coopa::scene::SceneObject* root, const std::string& path) {
    if (!root) return nullptr;
    if (path.empty()) return root;
    coopa::scene::SceneObject* cur = root;
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

/** @brief `t` folded into [0, length] per the clip's wrap mode (as the Animator plays it). */
inline float wrap_clip_time(const coopa::anim::AnimationClip& clip, float t) {
    const float len = clip.effective_length();
    if (len <= 0.0f) return std::max(t, 0.0f);
    switch (clip.wrap) {
        case coopa::anim::WrapMode::Once: return std::clamp(t, 0.0f, len);
        case coopa::anim::WrapMode::PingPong: {
            const float m = std::fmod(std::max(t, 0.0f), 2.0f * len);
            return m <= len ? m : 2.0f * len - m;
        }
        case coopa::anim::WrapMode::Loop:
        default: return t >= len ? std::fmod(t, len) : std::max(t, 0.0f);
    }
}

/**
 * @brief Writes the clip's pose at clip time `t` (not wrapped here) onto the objects under `root`.
 * @return How many tracks were applied.
 */
inline int apply_clip_pose(const coopa::anim::AnimationClip& clip, float t, coopa::scene::SceneObject* root) {
    auto& reg = coopa::anim::AnimatedPropertyRegistry::instance();
    int applied = 0;
    for (const auto& track : clip.tracks) {
        if (track.kind != coopa::anim::TrackKind::Keyframed || track.curve.empty()) continue;
        coopa::scene::SceneObject* obj = resolve_rig_path(root, track.object_path);
        if (!obj) continue;
        uint8_t mask = 0x0F;
        const coopa::anim::AnimatedProperty* prop = reg.find(track.component_type, track.property, &mask);
        if (!prop) continue;
        coopa::scene::Component* comp = obj->get_component_by_type_name(track.component_type, track.component_index);
        if (!comp) continue;
        void* target = prop->cast(*comp);
        if (!target) continue;
        float cur[4] = {0, 0, 0, 1}, val[4] = {0, 0, 0, 1};
        prop->get(target, cur);
        mask &= static_cast<uint8_t>((1u << prop->component_count) - 1u);
        if (mask == static_cast<uint8_t>((1u << prop->component_count) - 1u)) {
            track.curve.sample(t, prop->component_count, val);
        } else {
            // A channel suffix: the curve's channels map onto the masked ones, in order.
            float sampled[4] = {0, 0, 0, 0};
            track.curve.sample(t, 4, sampled);
            int k = 0;
            for (int c = 0; c < prop->component_count; ++c) val[c] = (mask >> c) & 1 ? sampled[k++] : cur[c];
        }
        prop->set(target, val);
        ++applied;
    }
    return applied;
}

}  // namespace toy::editor

#endif  // TOYEDITOR_ANIM_CLIP_POSE_H
