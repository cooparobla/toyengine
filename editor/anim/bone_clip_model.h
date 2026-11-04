/**
 * @file bone_clip_model.h
 * @brief The editor's model of one SKELETAL animation clip file (coopa::rig::SkeletalClip's YAML
 *        schema): key and read back per-bone channels.
 *
 * Pure (no GPU, no UI), the bone-addressed sibling of clip_model.h: the Timeline panel writes a
 * BoneClipModel back with to_node(); the runtime reads the same file through
 * coopa::rig::parse_skeletal_clip(). A track addresses a BONE NAME (not an object path) and
 * groups its three channels (location/rotation/scale) together, matching the asset's own
 * `bones: [{bone, location, rotation, scale}]` shape rather than ClipModel's flat track list.
 *
 * Rotation needs no authoring-time hemisphere flip: coopa::rig::sample_rotation() always slerps
 * the short way at SAMPLE time (see skeletal_clip.h), unlike the property side's per-channel
 * nlerp, which depends on the keys already agreeing in sign.
 *
 * Scope: set_key(), find/key lookup, a lossless YAML round trip (what Pose Mode's "I" needs), and
 * the dope-sheet cell ops ClipModel has (delete/move keys, easing, a KeyRef-equivalent) so the
 * Timeline's visual bone rows (see the create-animation plan's Stage 6 "Timeline" section) can
 * reuse the same key-selection/drag/easing-menu shape, addressed by (bone, channel) instead of
 * (object, property).
 */

#ifndef TOYEDITOR_ANIM_BONE_CLIP_MODEL_H
#define TOYEDITOR_ANIM_BONE_CLIP_MODEL_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <set>
#include <string>
#include <vector>

namespace toy::editor {

struct BoneClipKey {
    float time = 0.0f;
    glm::vec4 value{0.0f, 0.0f, 0.0f, 1.0f};
    std::string easing;   ///< "" = linear; else step / ease_in / ease_out / ease_in_out.
    bool operator==(const BoneClipKey& o) const { return time == o.time && value == o.value && easing == o.easing; }
};

/** @brief One channel's keys (location/rotation/scale); empty = untouched by this clip. */
struct BoneChannel {
    std::vector<BoneClipKey> keys;   ///< Sorted by time.
    bool operator==(const BoneChannel& o) const { return keys == o.keys; }
};

struct BoneClipTrack {
    std::string bone;        ///< A bone name in the rig this clip plays on.
    BoneChannel location;    ///< 3 floats.
    BoneChannel rotation;    ///< 4 floats (x, y, z, w); always slerped at sample time.
    BoneChannel scale;       ///< 3 floats.
    bool operator==(const BoneClipTrack& o) const {
        return bone == o.bone && location == o.location && rotation == o.rotation && scale == o.scale;
    }
};

/** @brief One clip event: a named marker the SkeletalAnimator fires when its playhead crosses `time`. */
struct BoneClipEvent {
    float time = 0.0f;
    std::string name = "event";
    std::string string_value;
    float float_value = 0.0f;
    bool operator==(const BoneClipEvent& o) const {
        return time == o.time && name == o.name && string_value == o.string_value && float_value == o.float_value;
    }
};

/** @brief A clip's `root_motion:` block: which BONE's travel is extracted as root motion. */
struct BoneRootMotionSpec {
    std::string bone;                    ///< Empty = disabled.
    std::string translation = "xy";      ///< xy | xyz | none
    bool yaw = false;                    ///< rotation: yaw
    bool operator==(const BoneRootMotionSpec& o) const { return bone == o.bone && translation == o.translation && yaw == o.yaw; }
};

/** @brief Values a channel carries: rotation 4, else 3. */
inline int bone_channel_components(const std::string& channel) { return channel == "rotation" ? 4 : 3; }

struct BoneClipModel {
    std::string name = "Clip";
    std::string wrap = "loop";    ///< loop / once / pingpong
    float length = 1.0f;          ///< Seconds (always written: the runtime needs it to wrap).
    std::vector<BoneClipTrack> bones;
    std::vector<BoneClipEvent> events;    ///< Sorted by time (stable).
    BoneRootMotionSpec root_motion;

    bool operator==(const BoneClipModel& o) const {
        return name == o.name && wrap == o.wrap && length == o.length && bones == o.bones && events == o.events &&
               root_motion == o.root_motion;
    }

    static constexpr float kTimeEps = 1e-4f;

    // --- YAML ---

    static BoneClipModel from_node(const Node& root);
    Node to_node() const;

    // --- keying ---

    BoneClipTrack* find_bone_track(const std::string& bone);
    const BoneClipTrack* find_bone_track(const std::string& bone) const {
        return const_cast<BoneClipModel*>(this)->find_bone_track(bone);
    }

    /** @brief Sets (or adds) the key of `bone`'s `channel` ("location"/"rotation"/"scale") at `time`. */
    void set_key(const std::string& bone, const std::string& channel, float time, glm::vec4 value);

    /** @brief The distinct key times of one bone's channel. */
    std::vector<float> key_times(const std::string& bone, const std::string& channel) const;

    /** @brief Every distinct key time of `bone` (across location/rotation/scale), sorted. */
    std::vector<float> key_times(const std::string& bone) const;

    /** @brief Every distinct key time in the clip (every bone, every channel), sorted. */
    std::vector<float> key_times() const;

    /** @brief One dope-sheet cell: a bone's keys at a time -- every channel (`channel` empty,
     *         the bone-name row) or one channel's when the row is expanded ("location", ...). */
    struct BoneKeyRef {
        std::string bone;
        float time = 0.0f;
        std::string channel;   ///< "" = every channel of the bone.
        bool operator<(const BoneKeyRef& o) const;
    };

    /** @brief Whether a key of `bone`'s `channel` at `time` is covered by `refs` (whole-bone or that channel's ref). */
    static bool covered(const std::set<BoneKeyRef>& refs, const std::string& bone, const std::string& channel, float time) {
        return refs.count({bone, time, ""}) > 0 || refs.count({bone, time, channel}) > 0;
    }

    /** @brief Sets the interpolation leaving each covered key ("" linear, step, ease_in, ease_out, ease_in_out). */
    size_t set_easing(const std::set<BoneKeyRef>& refs, const std::string& easing);

    /** @brief The interpolation shared by every covered key, or "mixed" / "" when none. */
    std::string easing_of(const std::set<BoneKeyRef>& refs) const;

    /** @brief The nearest key time strictly after (dir > 0) or before `t`, over every bone/channel; `t` if none. */
    float neighbour_key(float t, int dir) const;

    /** @brief Removes every key of each referenced (bone, channel, time); drops a channel left empty,
     *         and a track left with all three channels empty. */
    size_t delete_keys(const std::set<BoneKeyRef>& refs);

    /**
     * @brief Shifts each referenced (bone, channel, time)'s keys by `dt` (clamped at 0). A moved
     *        key landing on an unmoved one replaces it. Returns the moved cells' new references.
     */
    std::set<BoneKeyRef> move_keys(const std::set<BoneKeyRef>& refs, float dt);

    // --- events ---

    void sort_events_();
    size_t add_event(float time, const std::string& event_name);
    int move_event(size_t index, float time);
    bool delete_event(size_t index);
};

}  // namespace toy::editor

#endif  // TOYEDITOR_ANIM_BONE_CLIP_MODEL_H
