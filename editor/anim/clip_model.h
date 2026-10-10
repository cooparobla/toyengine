/**
 * @file clip_model.h
 * @brief The editor's model of one animation clip file (coopa::anim's YAML schema), and the
 *        dope-sheet operations on it: key, delete and move keys per object and time.
 *
 * Pure (no GPU, no UI): the Timeline panel (app/ui/timeline.inl) edits a ClipModel and
 * writes it back with to_node(); the runtime reads the same file through
 * coopa::anim::parse_clip(). Procedural tracks are carried through verbatim (the editor shows
 * them but does not key them).
 *
 * Rigs are object hierarchies: a track's `object` is a path from the rig root (the object with
 * the Animator) -- "" for the root itself, "Arm/Hand" below it -- exactly as the Animator
 * resolves it. Rotations are keyed as `rotation_quat` (see coopa's AnimatedPropertyRegistry):
 * set_key() flips a quaternion to the hemisphere of its neighbouring key so the runtime's
 * per-channel interpolation (nlerp) takes the short way round.
 *
 * Clip events (`events:`, coopa::anim::AnimationEvent) are first-class: the Timeline's Events
 * lane adds, drags, renames and deletes them. They stay sorted by time (stable), as the runtime
 * sorts them. A clip's `root_motion:` block is carried through as an unknown key (extra).
 */

#ifndef TOYEDITOR_ANIM_CLIP_MODEL_H
#define TOYEDITOR_ANIM_CLIP_MODEL_H

#include "../core/yaml_util.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace toy::editor {

struct ClipKey {
    float time = 0.0f;
    glm::vec4 value{0.0f, 0.0f, 0.0f, 1.0f};
    std::string easing;   ///< "" = linear; else step / ease_in / ease_out / ease_in_out.
    Node extra = Node::mapping();   ///< Keys this model does not know, written back verbatim.
    bool operator==(const ClipKey& o) const { return time == o.time && value == o.value && easing == o.easing && extra == o.extra; }
};

struct ClipTrack {
    std::string object;                    ///< Path from the rig root ("" = the root).
    std::string component = "Transform";
    std::string property;                  ///< e.g. position, rotation_quat, scale, position.x
    std::vector<ClipKey> keys;             ///< Sorted by time.
    Node procedural;                       ///< A procedural track's node, kept verbatim (keys empty).
    Node extra = Node::mapping();          ///< Unknown track keys (component_index, ...), written back verbatim.
    bool is_procedural() const { return !procedural.is_null(); }
    bool operator==(const ClipTrack& o) const;
};

/** @brief One clip event: a named marker the Animator fires when its playhead crosses `time`. */
struct ClipEvent {
    float time = 0.0f;
    std::string name = "event";
    std::string string_value;       ///< `string:` (omitted when empty).
    float float_value = 0.0f;       ///< `float:` (omitted when 0).
    Node extra = Node::mapping();   ///< Unknown event keys, written back verbatim.
    bool operator==(const ClipEvent& o) const;
};

/** @brief Values a property carries (position: 3, rotation_quat: 4, ...); suffixes count their channels. */
int clip_property_components(const std::string& property);

struct ClipModel {
    std::string name = "Clip";
    std::string wrap = "loop";    ///< loop / once / pingpong
    float length = 1.0f;          ///< Seconds (always written: the runtime needs it to wrap).
    std::vector<ClipTrack> tracks;
    std::vector<ClipEvent> events;  ///< Sorted by time (stable).
    Node extra = Node::mapping();   ///< Unknown `clip:` keys (and other top-level keys under `__root`), written back verbatim.

    bool operator==(const ClipModel& o) const;

    /** @brief The keys of `n` not in `known`, as a mapping. */
    static Node extras_of(const Node& n, std::initializer_list<const char*> known);
    static void put_extras(Node& n, const Node& extra);

    static constexpr float kTimeEps = 1e-4f;   ///< Keys closer than this are the same key.

    // --- YAML ---

    static ClipModel from_node(const Node& root);

    Node to_node() const;

    // --- dope sheet ---

    ClipTrack* find_track(const std::string& object, const std::string& property, const std::string& component = "Transform");
    const ClipTrack* find_track(const std::string& object, const std::string& property,
                                const std::string& component = "Transform") const {
        return const_cast<ClipModel*>(this)->find_track(object, property, component);
    }

    /**
     * @brief Sets (or adds) the key of `object`.`property` at `time`. A rotation_quat value is
     *        flipped to its neighbouring key's hemisphere (q and -q are the same rotation; the
     *        runtime interpolates channel by channel, so the pair must agree in sign).
     */
    void set_key(const std::string& object, const std::string& property, float time, glm::vec4 value,
                 const std::string& component = "Transform");

    /** @brief Every distinct key time of `object` (all objects if `object` is null), sorted. */
    std::vector<float> key_times(const std::string* object = nullptr) const;

    /** @brief One dope-sheet cell: an object's keys at a time ("" object... the root). */
    /**
     * @brief One dope-sheet cell: an object's keys at a time -- all its tracks (`property`
     *        empty), or one channel's when the row is expanded (`property` = "position", ...).
     */
    struct KeyRef {
        std::string object;
        float time = 0.0f;
        std::string property;   ///< "" = every track of the object.
        bool operator<(const KeyRef& o) const;
    };

    /** @brief Whether a key of track `t` at `time` is covered by `refs` (whole-object or that channel's ref). */
    static bool covered(const std::set<KeyRef>& refs, const ClipTrack& t, float time) {
        return refs.count({t.object, time, ""}) > 0 || refs.count({t.object, time, t.property}) > 0;
    }

    /** @brief The distinct key times of one track of `object`. */
    std::vector<float> key_times(const std::string& object, const std::string& property) const;

    /** @brief Sets the interpolation leaving each covered key ("" linear, step, ease_in, ease_out, ease_in_out). */
    size_t set_easing(const std::set<KeyRef>& refs, const std::string& easing);

    /** @brief The interpolation shared by every covered key, or "mixed" / "" when none. */
    std::string easing_of(const std::set<KeyRef>& refs) const;

    /** @brief The nearest key time strictly after (dir > 0) or before `t`, over every track; `t` if none. */
    float neighbour_key(float t, int dir) const;

    /** @brief Removes every key of each referenced (object, time); drops tracks left empty. */
    size_t delete_keys(const std::set<KeyRef>& refs);

    /**
     * @brief Shifts each referenced (object, time)'s keys by `dt` (clamped at 0). A moved key
     *        landing on an unmoved one replaces it. Returns the moved cells' new references.
     */
    std::set<KeyRef> move_keys(const std::set<KeyRef>& refs, float dt);

    // --- events ---

    void sort_events_();

    /** @brief Adds an event at `time` (clamped at 0), after any already there; returns its index. */
    size_t add_event(float time, const std::string& event_name);

    /** @brief Moves event `index` to `time` (clamped at 0); returns its new index (-1 if none). */
    int move_event(size_t index, float time);

    /** @brief Removes event `index`; false if out of range. */
    bool delete_event(size_t index);

    /** @brief Renames a track object path (an object renamed or moved in the rig), including its descendants'. */
    void rename_object(const std::string& from, const std::string& to);
};

}  // namespace toy::editor

#endif  // TOYEDITOR_ANIM_CLIP_MODEL_H
