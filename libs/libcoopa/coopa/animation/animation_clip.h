/**
 * @file animation_clip.h
 * @brief Immutable animation data: a named set of tracks, each either
 *        keyframed or procedural, targeting a component field by name.
 */

#ifndef COOPA_ANIMATION_ANIMATION_CLIP_H
#define COOPA_ANIMATION_ANIMATION_CLIP_H

#include <coopa/animation/animation_curve.h>
#include <coopa/animation/procedural_track.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coopa {
namespace anim {

/**
 * @enum WrapMode
 * @brief How a clip's time behaves once it passes the clip's length.
 */
enum class WrapMode : uint8_t {
    Once,     ///< Clamps at length; holds the final pose. Fires Animator::on_state_finished once.
    Loop,     ///< Wraps back to 0.
    PingPong, ///< Reflects back and forth between 0 and length.
};

/** @brief Parses a WrapMode from its YAML spelling ("once"/"loop"/"pingpong"). Defaults to Loop. */
inline WrapMode parse_wrap_mode(const std::string& s) {
    if (s == "once") return WrapMode::Once;
    if (s == "pingpong") return WrapMode::PingPong;
    return WrapMode::Loop;
}

/**
 * @enum TrackKind
 * @brief Whether a track samples a keyframed AnimationCurve or a procedural evaluator.
 */
enum class TrackKind : uint8_t { Keyframed = 0, Procedural };

/**
 * @struct AnimationTrack
 * @brief One clip-authored binding target plus its value source.
 *
 * object_path/component_type/component_index/property/channel_mask together
 * describe WHAT to animate (resolved against the scene once, at an
 * Animator's bind time — see animator.h). kind/curve/procedural describe HOW
 * its value is produced each frame.
 */
struct AnimationTrack {
    /**
     * Empty (or omitted) means the Animator's own owner. Contains '/' means a
     * descendant path walked segment-by-segment ("Content/Row/Icon"). Any
     * other non-empty value is looked up by name, first as a descendant of
     * the Animator's owner, then scene-wide.
     */
    std::string object_path;
    std::string component_type = "Transform"; ///< Matches Component::type_name().
    int         component_index = 0;          ///< Disambiguates two same-typed components on one object.

    /** Registry key into AnimatedPropertyRegistry, with an optional channel
     *  suffix (e.g. "anchored_position.y") stripped and decoded at bind time. */
    std::string property;

    TrackKind        kind = TrackKind::Keyframed;
    AnimationCurve   curve;      ///< Valid when kind == Keyframed.
    ProceduralTrack  procedural; ///< Valid when kind == Procedural.
};

/**
 * @struct AnimationEvent
 * @brief A named marker on a clip's timeline, fired by an Animator when its playhead crosses `time`.
 *
 * Delivered through Animator::on_event and posted to the scene EventBus as signal
 * "anim_event" (object = the Animator's owner name; args name / string / float / state).
 * The payload fields are free-form: a footstep might carry `string_value: left`.
 */
struct AnimationEvent {
    float       time = 0.0f;   ///< Seconds into the clip, in [0, length].
    std::string name;
    std::string string_value;
    float       float_value = 0.0f;
};

/**
 * @enum RootMotionTranslation
 * @brief Which channels of the root bone's position a clip hands over as root motion.
 */
enum class RootMotionTranslation : uint8_t {
    None, ///< Translation stays on the bone.
    XY,   ///< Ground-plane travel (the engine is Z-up); vertical bob stays on the bone.
    XYZ,  ///< All three channels.
};

/** @brief Parses a RootMotionTranslation ("xy" / "xyz" / "none"). Defaults to XY. */
inline RootMotionTranslation parse_root_motion_translation(const std::string& s) {
    if (s == "none") return RootMotionTranslation::None;
    if (s == "xyz") return RootMotionTranslation::XYZ;
    return RootMotionTranslation::XY;
}

/**
 * @struct RootMotionSpec
 * @brief A clip's `root_motion:` block: which bone's travel is extracted as root motion.
 *
 * The root bone's keyframed `position` (or `position.<channels>`) and `rotation_quat` tracks
 * are sampled at the previous and new playback times every frame; the difference is the
 * Animator's root_motion_delta(), and the extracted channels are held at their clip-start
 * value on the bone (so the mesh stays over its owner while the owner travels).
 */
struct RootMotionSpec {
    std::string           object;                                   ///< Bone path, resolved like a track's object_path. Empty = disabled.
    RootMotionTranslation translation = RootMotionTranslation::XY;
    bool                  yaw = false;                              ///< Extract rotation about +Z (`rotation: yaw`).

    bool enabled() const {
        return !object.empty() && (translation != RootMotionTranslation::None || yaw);
    }
};

/**
 * @class AnimationClip
 * @brief Immutable, shareable animation data — the payload behind AssetHandle<AnimationClip>.
 *
 * Pure data: no SceneObject*, no Component*, no registry lookups. Safe to
 * decode entirely on an asset IO worker thread (see animation_clip_loader.h)
 * and to sample from many Animators, including from worker threads inside
 * Animator::evaluate_(). All per-instance playback state (time, speed,
 * bindings) lives on the Animator, never here.
 */
class AnimationClip {
public:
    std::string            name;
    WrapMode                wrap = WrapMode::Loop;
    std::vector<AnimationTrack> tracks;
    std::vector<AnimationEvent> events;      ///< Sorted by time (see sort_events()).
    RootMotionSpec              root_motion;

    /** @brief Orders events by time (stable, so same-time events keep authoring order). */
    void sort_events() {
        std::stable_sort(events.begin(), events.end(),
                         [](const AnimationEvent& a, const AnimationEvent& b) { return a.time < b.time; });
    }

    /**
     * @brief Sets an explicit length (e.g. from the clip's `length:` YAML field).
     *
     * Overrides the length that would otherwise be derived from the longest
     * keyframed track. Required for a clip made up entirely of procedural
     * tracks, which have no intrinsic length of their own.
     */
    void set_explicit_length(float length) {
        explicit_length_ = length;
        has_explicit_length_ = true;
    }

    /**
     * @brief The clip's length: the explicit length if set, else the longest
     *        keyframed track's length, else 0 (meaning "infinite" — see below).
     *
     * Procedural tracks contribute nothing to the derived maximum; they have
     * no intrinsic length. A clip made up only of procedural tracks with no
     * explicit `length:` therefore reports 0, which Animator treats as
     * infinite: time is never wrapped and wrap is ignored regardless of its
     * declared value, since there's nothing to wrap against.
     */
    float effective_length() const {
        if (has_explicit_length_) return explicit_length_;
        float max_len = 0.0f;
        for (const auto& t : tracks) {
            if (t.kind == TrackKind::Keyframed) {
                max_len = std::max(max_len, t.curve.length());
            }
        }
        return max_len;
    }

    /** @brief True when this clip has no way to derive a finite length (see effective_length()'s doc). */
    bool is_infinite() const { return effective_length() <= 0.0f; }

private:
    float explicit_length_ = 0.0f;
    bool  has_explicit_length_ = false;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ANIMATION_CLIP_H
