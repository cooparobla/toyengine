/**
 * @file skeletal_clip.h
 * @brief Immutable skeletal animation data: a named set of per-bone channel curves.
 *
 * Reuses coopa::anim's curve, keyframe, wrap-mode and event types directly -- a skeletal
 * clip differs from a property AnimationClip only in WHAT a track addresses (a bone name
 * in a rig, not an object path/component/property) and in how rotation is sampled (slerp,
 * not the per-channel lerp coopa::anim::AnimationCurve::sample() does).
 */

#ifndef COOPA_RIG_SKELETAL_CLIP_H
#define COOPA_RIG_SKELETAL_CLIP_H

#include <coopa/animation/animation_clip.h>
#include <coopa/animation/animation_curve.h>
#include <coopa/animation/keyframe.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace coopa {
namespace rig {

/**
 * @struct SkeletalTrack
 * @brief One bone's keyframed channels. An empty curve means that channel is left untouched
 *        by this clip -- a pose authored elsewhere (another crossfading clip, a hand pose,
 *        an IK solve) shows through.
 */
struct SkeletalTrack {
    std::string                  bone; ///< A bone name in the rig this clip plays on.
    coopa::anim::AnimationCurve  location; ///< 3 floats; componentwise lerp (translation has no hemisphere problem).
    coopa::anim::AnimationCurve  rotation; ///< 4 floats (x, y, z, w); sampled by slerp -- see sample_rotation().
    coopa::anim::AnimationCurve  scale;    ///< 3 floats.
};

/**
 * @struct SkeletalRootMotionSpec
 * @brief A clip's `root_motion:` block: which bone's travel is extracted as root motion.
 *
 * Identical in spirit to coopa::anim::RootMotionSpec, but names a BONE rather than an
 * object path -- a skeletal clip has no object paths at all.
 */
struct SkeletalRootMotionSpec {
    std::string                         bone; ///< Bone name. Empty = disabled.
    coopa::anim::RootMotionTranslation  translation = coopa::anim::RootMotionTranslation::XY;
    bool                                yaw = false; ///< Extract rotation about +Z (`rotation: yaw`).

    bool enabled() const {
        return !bone.empty() && (translation != coopa::anim::RootMotionTranslation::None || yaw);
    }
};

/**
 * @class SkeletalClip
 * @brief Immutable, shareable skeletal animation data — the payload behind AssetHandle<SkeletalClip>.
 *
 * Pure data: no Rig*, no bone index (bone lookup happens once, at a SkeletalAnimator's bind
 * time — see skeletal_animator.h). Safe to decode entirely on an asset IO worker thread.
 */
class SkeletalClip {
public:
    std::string                              name;
    coopa::anim::WrapMode                    wrap = coopa::anim::WrapMode::Loop;
    std::vector<SkeletalTrack>               bones;
    std::vector<coopa::anim::AnimationEvent> events; ///< Sorted by time (see sort_events()).
    SkeletalRootMotionSpec                   root_motion;

    /** @brief Orders events by time (stable, so same-time events keep authoring order). */
    void sort_events() {
        std::stable_sort(events.begin(), events.end(),
                         [](const coopa::anim::AnimationEvent& a, const coopa::anim::AnimationEvent& b) { return a.time < b.time; });
    }

    /** @brief Sets an explicit length (e.g. from the clip's `length:` YAML field). */
    void set_explicit_length(float length) {
        explicit_length_ = length;
        has_explicit_length_ = true;
    }

    /** @brief The clip's length: explicit if set, else the longest channel curve's length, else 0 ("infinite"). */
    float effective_length() const {
        if (has_explicit_length_) return explicit_length_;
        float max_len = 0.0f;
        for (const auto& t : bones) {
            max_len = std::max(max_len, t.location.length());
            max_len = std::max(max_len, t.rotation.length());
            max_len = std::max(max_len, t.scale.length());
        }
        return max_len;
    }

    /** @brief True when this clip has no way to derive a finite length. */
    bool is_infinite() const { return effective_length() <= 0.0f; }

private:
    float explicit_length_ = 0.0f;
    bool  has_explicit_length_ = false;
};

/**
 * @brief Samples a rotation curve by slerp, with hemisphere correction applied at sample
 *        time rather than relied on at authoring time.
 *
 * Unlike coopa::anim's `rotation_quat` AnimatedProperty (componentwise lerp then
 * normalize, correct only "as long as consecutive keys share a hemisphere" — see
 * coopa/animation/animated_property.h), this always takes the short arc between two
 * keys regardless of how they were authored.
 */
inline glm::quat sample_rotation(const coopa::anim::AnimationCurve& curve, float time) {
    const auto& keys = curve.keys();
    if (keys.empty()) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    auto to_quat = [](const coopa::anim::Keyframe& k) { return glm::quat(k.value[3], k.value[0], k.value[1], k.value[2]); };
    if (keys.size() == 1 || time <= keys.front().time) return glm::normalize(to_quat(keys.front()));
    if (time >= keys.back().time) return glm::normalize(to_quat(keys.back()));

    auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float t, const coopa::anim::Keyframe& k) { return t < k.time; });
    const coopa::anim::Keyframe& next = *it;
    const coopa::anim::Keyframe& prev = *(it - 1);

    const float span = next.time - prev.time;
    const float raw_t = (span > 0.0f) ? (time - prev.time) / span : 0.0f;
    const float t = coopa::anim::apply_easing(prev.easing, raw_t);

    glm::quat a = glm::normalize(to_quat(prev));
    glm::quat b = glm::normalize(to_quat(next));
    if (glm::dot(a, b) < 0.0f) b = -b; // shortest arc, regardless of how the keys were authored
    return glm::normalize(glm::slerp(a, b, t));
}

} // namespace rig
} // namespace coopa

#endif // COOPA_RIG_SKELETAL_CLIP_H
