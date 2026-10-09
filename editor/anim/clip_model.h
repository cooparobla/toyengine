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
    bool operator==(const ClipTrack& o) const {
        return object == o.object && component == o.component && property == o.property && keys == o.keys &&
               is_procedural() == o.is_procedural() && extra == o.extra && (!is_procedural() || procedural == o.procedural);
    }
};

/** @brief One clip event: a named marker the Animator fires when its playhead crosses `time`. */
struct ClipEvent {
    float time = 0.0f;
    std::string name = "event";
    std::string string_value;       ///< `string:` (omitted when empty).
    float float_value = 0.0f;       ///< `float:` (omitted when 0).
    Node extra = Node::mapping();   ///< Unknown event keys, written back verbatim.
    bool operator==(const ClipEvent& o) const {
        return time == o.time && name == o.name && string_value == o.string_value && float_value == o.float_value && extra == o.extra;
    }
};

/** @brief Values a property carries (position: 3, rotation_quat: 4, ...); suffixes count their channels. */
inline int clip_property_components(const std::string& property) {
    const size_t dot = property.find('.');
    if (dot != std::string::npos) return static_cast<int>(property.size() - dot - 1);
    if (property == "rotation_quat") return 4;
    if (property == "position" || property == "rotation" || property == "scale") return 3;
    return 4;
}

struct ClipModel {
    std::string name = "Clip";
    std::string wrap = "loop";    ///< loop / once / pingpong
    float length = 1.0f;          ///< Seconds (always written: the runtime needs it to wrap).
    std::vector<ClipTrack> tracks;
    std::vector<ClipEvent> events;  ///< Sorted by time (stable).
    Node extra = Node::mapping();   ///< Unknown `clip:` keys (and other top-level keys under `__root`), written back verbatim.

    bool operator==(const ClipModel& o) const {
        return name == o.name && wrap == o.wrap && length == o.length && tracks == o.tracks && events == o.events && extra == o.extra;
    }

    /** @brief The keys of `n` not in `known`, as a mapping. */
    static Node extras_of(const Node& n, std::initializer_list<const char*> known) {
        Node out = Node::mapping();
        if (!n.is_mapping()) return out;
        for (const auto& kv : n.as_map()) {
            const std::string k = kv.first.get_value<std::string>();
            bool is_known = false;
            for (const char* kk : known) is_known |= k == kk;
            if (!is_known) out[k] = kv.second;
        }
        return out;
    }
    static void put_extras(Node& n, const Node& extra) {
        if (extra.is_mapping()) for (const auto& kv : extra.as_map()) n[kv.first.get_value<std::string>()] = kv.second;
    }

    static constexpr float kTimeEps = 1e-4f;   ///< Keys closer than this are the same key.

    // --- YAML ---

    static ClipModel from_node(const Node& root) {
        ClipModel m;
        if (!root.is_mapping() || !root.contains("clip")) return m;
        const Node& c = root.at("clip");
        m.extra = extras_of(c, {"name", "wrap", "length", "tracks", "events"});
        if (c.contains("events") && c.at("events").is_sequence()) {
            for (const auto& en : c.at("events").as_seq()) {
                if (!en.is_mapping()) continue;
                ClipEvent e;
                e.time = get_float(en, "time", 0.0f);
                e.name = get_string(en, "name", "");
                e.string_value = get_string(en, "string", "");
                e.float_value = get_float(en, "float", 0.0f);
                e.extra = extras_of(en, {"time", "name", "string", "float"});
                m.events.push_back(std::move(e));
            }
            m.sort_events_();
        }
        m.name = get_string(c, "name", m.name);
        m.wrap = get_string(c, "wrap", m.wrap);
        float max_t = 0.0f;
        if (c.contains("tracks")) {
            for (const auto& tn : c.at("tracks").as_seq()) {
                ClipTrack t;
                t.extra = extras_of(tn, {"object", "component", "property", "keys", "procedural"});
                t.object = get_string(tn, "object", "");
                t.component = get_string(tn, "component", "Transform");
                t.property = get_string(tn, "property", "");
                if (tn.contains("procedural")) {
                    t.procedural = tn;          // the whole track node, verbatim
                    t.extra = Node::mapping();
                } else if (tn.contains("keys")) {
                    for (const auto& kn : tn.at("keys").as_seq()) {
                        ClipKey k;
                        k.value = glm::vec4(0.0f);   // channels past the property's count stay 0 (canonical)
                        k.time = get_float(kn, "time", 0.0f);
                        k.extra = extras_of(kn, {"time", "value", "easing"});
                        k.easing = get_string(kn, "easing", "");
                        if (k.easing == "linear") k.easing.clear();
                        if (kn.contains("value")) {
                            const Node& v = kn.at("value");
                            if (v.is_sequence()) {
                                const auto& sq = v.as_seq();
                                for (size_t i = 0; i < 4 && i < sq.size(); ++i) k.value[static_cast<int>(i)] = as_float(sq[i]);
                            } else if (v.is_mapping()) {
                                const char* names[4] = {"x", "y", "z", "w"};
                                for (int i = 0; i < 4; ++i) if (v.contains(names[i])) k.value[i] = as_float(v.at(names[i]));
                            } else {
                                k.value.x = as_float(v);
                            }
                        }
                        t.keys.push_back(k);
                        max_t = std::max(max_t, k.time);
                    }
                    std::sort(t.keys.begin(), t.keys.end(), [](const ClipKey& a, const ClipKey& b) { return a.time < b.time; });
                }
                m.tracks.push_back(std::move(t));
            }
        }
        m.length = c.contains("length") ? get_float(c, "length", 1.0f) : std::max(max_t, 1.0f);
        return m;
    }

    Node to_node() const {
        Node c = Node::mapping();
        c["name"] = Node(name);
        c["wrap"] = Node(wrap);
        c["length"] = make_float(length);
        Node ts = Node::sequence();
        for (const auto& t : tracks) {
            if (t.is_procedural()) { ts.as_seq().push_back(t.procedural); continue; }
            Node tn = Node::mapping();
            tn["object"] = Node(t.object);
            if (t.component != "Transform") tn["component"] = Node(t.component);
            tn["property"] = Node(t.property);
            Node ks = Node::sequence();
            const int n = clip_property_components(t.property);
            for (const auto& k : t.keys) {
                Node kn = Node::mapping();
                kn["time"] = make_float(k.time);
                Node v = Node::sequence();
                for (int i = 0; i < n; ++i) v.as_seq().push_back(make_float(k.value[i]));
                kn["value"] = v;
                if (!k.easing.empty()) kn["easing"] = Node(k.easing);
                put_extras(kn, k.extra);
                ks.as_seq().push_back(kn);
            }
            tn["keys"] = ks;
            put_extras(tn, t.extra);
            ts.as_seq().push_back(tn);
        }
        c["tracks"] = ts;
        if (!events.empty()) {
            Node es = Node::sequence();
            for (const auto& e : events) {
                Node en = Node::mapping();
                en["time"] = make_float(e.time);
                en["name"] = Node(e.name);
                if (!e.string_value.empty()) en["string"] = Node(e.string_value);
                if (e.float_value != 0.0f) en["float"] = make_float(e.float_value);
                put_extras(en, e.extra);
                es.as_seq().push_back(en);
            }
            c["events"] = es;
        }
        put_extras(c, extra);
        Node root = Node::mapping();
        root["clip"] = c;
        return root;
    }

    // --- dope sheet ---

    ClipTrack* find_track(const std::string& object, const std::string& property, const std::string& component = "Transform") {
        for (auto& t : tracks) if (t.object == object && t.property == property && t.component == component && !t.is_procedural()) return &t;
        return nullptr;
    }
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
                 const std::string& component = "Transform") {
        ClipTrack* t = find_track(object, property, component);
        if (!t) {
            tracks.push_back({object, component, property, {}, Node()});
            t = &tracks.back();
        }
        for (int c = clip_property_components(property); c < 4; ++c) value[c] = 0.0f;   // canonical: unused channels 0
        if (property == "rotation_quat" && !t->keys.empty()) {
            const ClipKey* nb = nullptr;
            for (const auto& k : t->keys) if (k.time < time - kTimeEps) nb = &k;
            if (!nb) for (const auto& k : t->keys) if (k.time > time + kTimeEps) { nb = &k; break; }
            if (nb && glm::dot(nb->value, value) < 0.0f) value = -value;
        }
        for (auto& k : t->keys) {
            if (std::abs(k.time - time) <= kTimeEps) { k.value = value; return; }
        }
        t->keys.push_back({time, value, ""});
        std::sort(t->keys.begin(), t->keys.end(), [](const ClipKey& a, const ClipKey& b) { return a.time < b.time; });
    }

    /** @brief Every distinct key time of `object` (all objects if `object` is null), sorted. */
    std::vector<float> key_times(const std::string* object = nullptr) const {
        std::vector<float> out;
        for (const auto& t : tracks) {
            if (object && t.object != *object) continue;
            for (const auto& k : t.keys) {
                bool dup = false;
                for (float x : out) dup |= std::abs(x - k.time) <= kTimeEps;
                if (!dup) out.push_back(k.time);
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /** @brief One dope-sheet cell: an object's keys at a time ("" object... the root). */
    /**
     * @brief One dope-sheet cell: an object's keys at a time -- all its tracks (`property`
     *        empty), or one channel's when the row is expanded (`property` = "position", ...).
     */
    struct KeyRef {
        std::string object;
        float time = 0.0f;
        std::string property;   ///< "" = every track of the object.
        bool operator<(const KeyRef& o) const {
            if (object != o.object) return object < o.object;
            if (std::abs(time - o.time) > kTimeEps) return time < o.time;
            return property < o.property;
        }
    };

    /** @brief Whether a key of track `t` at `time` is covered by `refs` (whole-object or that channel's ref). */
    static bool covered(const std::set<KeyRef>& refs, const ClipTrack& t, float time) {
        return refs.count({t.object, time, ""}) > 0 || refs.count({t.object, time, t.property}) > 0;
    }

    /** @brief The distinct key times of one track of `object`. */
    std::vector<float> key_times(const std::string& object, const std::string& property) const {
        std::vector<float> out;
        for (const auto& t : tracks) {
            if (t.object != object || t.property != property) continue;
            for (const auto& k : t.keys) out.push_back(k.time);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /** @brief Sets the interpolation leaving each covered key ("" linear, step, ease_in, ease_out, ease_in_out). */
    size_t set_easing(const std::set<KeyRef>& refs, const std::string& easing) {
        size_t n = 0;
        for (auto& t : tracks) {
            for (auto& k : t.keys) {
                if (!covered(refs, t, k.time)) continue;
                k.easing = easing == "linear" ? std::string() : easing;
                ++n;
            }
        }
        return n;
    }

    /** @brief The interpolation shared by every covered key, or "mixed" / "" when none. */
    std::string easing_of(const std::set<KeyRef>& refs) const {
        std::string e;
        bool any = false;
        for (const auto& t : tracks) {
            for (const auto& k : t.keys) {
                if (!covered(refs, t, k.time)) continue;
                const std::string ke = k.easing.empty() ? "linear" : k.easing;
                if (any && ke != e) return "mixed";
                e = ke;
                any = true;
            }
        }
        return e;
    }

    /** @brief The nearest key time strictly after (dir > 0) or before `t`, over every track; `t` if none. */
    float neighbour_key(float t, int dir) const {
        float best = t;
        bool found = false;
        for (float k : key_times()) {
            if (dir > 0 && k > t + kTimeEps && (!found || k < best)) { best = k; found = true; }
            if (dir < 0 && k < t - kTimeEps && (!found || k > best)) { best = k; found = true; }
        }
        return best;
    }

    /** @brief Removes every key of each referenced (object, time); drops tracks left empty. */
    size_t delete_keys(const std::set<KeyRef>& refs) {
        size_t n = 0;
        for (auto& t : tracks) {
            const size_t before = t.keys.size();
            t.keys.erase(std::remove_if(t.keys.begin(), t.keys.end(), [&](const ClipKey& k) {
                return covered(refs, t, k.time);
            }), t.keys.end());
            n += before - t.keys.size();
        }
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [](const ClipTrack& t) { return !t.is_procedural() && t.keys.empty(); }),
                     tracks.end());
        return n;
    }

    /**
     * @brief Shifts each referenced (object, time)'s keys by `dt` (clamped at 0). A moved key
     *        landing on an unmoved one replaces it. Returns the moved cells' new references.
     */
    std::set<KeyRef> move_keys(const std::set<KeyRef>& refs, float dt) {
        std::set<KeyRef> moved;
        for (auto& t : tracks) {
            std::vector<ClipKey> keep, shifted;
            for (const auto& k : t.keys) {
                if (covered(refs, t, k.time)) {
                    ClipKey s = k;
                    s.time = std::max(0.0f, k.time + dt);
                    shifted.push_back(s);
                    moved.insert({t.object, s.time, refs.count({t.object, k.time, ""}) ? std::string() : t.property});
                } else {
                    keep.push_back(k);
                }
            }
            if (shifted.empty()) continue;
            for (const auto& s : shifted) {
                keep.erase(std::remove_if(keep.begin(), keep.end(), [&](const ClipKey& k) { return std::abs(k.time - s.time) <= kTimeEps; }),
                           keep.end());
            }
            keep.insert(keep.end(), shifted.begin(), shifted.end());
            std::sort(keep.begin(), keep.end(), [](const ClipKey& a, const ClipKey& b) { return a.time < b.time; });
            t.keys = std::move(keep);
        }
        return moved;
    }

    // --- events ---

    void sort_events_() {
        std::stable_sort(events.begin(), events.end(), [](const ClipEvent& a, const ClipEvent& b) { return a.time < b.time; });
    }

    /** @brief Adds an event at `time` (clamped at 0), after any already there; returns its index. */
    size_t add_event(float time, const std::string& event_name) {
        ClipEvent e;
        e.time = std::max(0.0f, time);
        e.name = event_name;
        size_t at = 0;
        while (at < events.size() && events[at].time <= e.time) ++at;
        events.insert(events.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
        return at;
    }

    /** @brief Moves event `index` to `time` (clamped at 0); returns its new index (-1 if none). */
    int move_event(size_t index, float time) {
        if (index >= events.size()) return -1;
        ClipEvent e = events[index];
        events.erase(events.begin() + static_cast<std::ptrdiff_t>(index));
        e.time = std::max(0.0f, time);
        // Insert after every event at or before the new time (stable relative order).
        size_t at = 0;
        while (at < events.size() && events[at].time <= e.time) ++at;
        events.insert(events.begin() + static_cast<std::ptrdiff_t>(at), e);
        return static_cast<int>(at);
    }

    /** @brief Removes event `index`; false if out of range. */
    bool delete_event(size_t index) {
        if (index >= events.size()) return false;
        events.erase(events.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    /** @brief Renames a track object path (an object renamed or moved in the rig), including its descendants'. */
    void rename_object(const std::string& from, const std::string& to) {
        for (auto& t : tracks) {
            if (t.object == from) t.object = to;
            else if (!from.empty() && t.object.rfind(from + "/", 0) == 0) t.object = to + t.object.substr(from.size());
        }
    }
};

}  // namespace toy::editor

#endif  // TOYEDITOR_ANIM_CLIP_MODEL_H
