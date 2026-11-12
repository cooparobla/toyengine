#include "editor/anim/clip_model.h"

namespace toy {
namespace editor {

bool ClipTrack::operator==(const ClipTrack& o) const {
    return object == o.object && component == o.component && property == o.property && keys == o.keys &&
           is_procedural() == o.is_procedural() && extra == o.extra && (!is_procedural() || procedural == o.procedural);
}

bool ClipEvent::operator==(const ClipEvent& o) const {
    return time == o.time && name == o.name && string_value == o.string_value && float_value == o.float_value && extra == o.extra;
}

int clip_property_components(const std::string& property) {
    const size_t dot = property.find('.');
    if (dot != std::string::npos) return static_cast<int>(property.size() - dot - 1);
    if (property == "rotation_quat") return 4;
    if (property == "position" || property == "rotation" || property == "scale") return 3;
    return 4;
}

bool ClipModel::operator==(const ClipModel& o) const {
    return name == o.name && wrap == o.wrap && length == o.length && tracks == o.tracks && events == o.events && extra == o.extra;
}

Node ClipModel::extras_of(const Node& n, std::initializer_list<const char*> known) {
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

void ClipModel::put_extras(Node& n, const Node& extra) {
    if (extra.is_mapping()) for (const auto& kv : extra.as_map()) n[kv.first.get_value<std::string>()] = kv.second;
}

ClipModel ClipModel::from_node(const Node& root) {
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

Node ClipModel::to_node() const {
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

ClipTrack* ClipModel::find_track(const std::string& object, const std::string& property, const std::string& component) {
    for (auto& t : tracks) if (t.object == object && t.property == property && t.component == component && !t.is_procedural()) return &t;
    return nullptr;
}

void ClipModel::set_key(const std::string& object, const std::string& property, float time, glm::vec4 value,
             const std::string& component) {
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

std::vector<float> ClipModel::key_times(const std::string* object) const {
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

bool ClipModel::KeyRef::operator<(const KeyRef& o) const {
    if (object != o.object) return object < o.object;
    if (std::abs(time - o.time) > kTimeEps) return time < o.time;
    return property < o.property;
}

std::vector<float> ClipModel::key_times(const std::string& object, const std::string& property) const {
    std::vector<float> out;
    for (const auto& t : tracks) {
        if (t.object != object || t.property != property) continue;
        for (const auto& k : t.keys) out.push_back(k.time);
    }
    std::sort(out.begin(), out.end());
    return out;
}

size_t ClipModel::set_easing(const std::set<KeyRef>& refs, const std::string& easing) {
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

std::string ClipModel::easing_of(const std::set<KeyRef>& refs) const {
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

float ClipModel::neighbour_key(float t, int dir) const {
    float best = t;
    bool found = false;
    for (float k : key_times()) {
        if (dir > 0 && k > t + kTimeEps && (!found || k < best)) { best = k; found = true; }
        if (dir < 0 && k < t - kTimeEps && (!found || k > best)) { best = k; found = true; }
    }
    return best;
}

size_t ClipModel::delete_keys(const std::set<KeyRef>& refs) {
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

std::set<ClipModel::KeyRef> ClipModel::move_keys(const std::set<KeyRef>& refs, float dt) {
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

void ClipModel::sort_events_() {
    std::stable_sort(events.begin(), events.end(), [](const ClipEvent& a, const ClipEvent& b) { return a.time < b.time; });
}

size_t ClipModel::add_event(float time, const std::string& event_name) {
    ClipEvent e;
    e.time = std::max(0.0f, time);
    e.name = event_name;
    size_t at = 0;
    while (at < events.size() && events[at].time <= e.time) ++at;
    events.insert(events.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
    return at;
}

int ClipModel::move_event(size_t index, float time) {
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

bool ClipModel::delete_event(size_t index) {
    if (index >= events.size()) return false;
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void ClipModel::rename_object(const std::string& from, const std::string& to) {
    for (auto& t : tracks) {
        if (t.object == from) t.object = to;
        else if (!from.empty() && t.object.rfind(from + "/", 0) == 0) t.object = to + t.object.substr(from.size());
    }
}

} // namespace editor
} // namespace toy
