#include "editor/anim/bone_clip_model.h"

#include <algorithm>
#include <cmath>

namespace toy {
namespace editor {

namespace {

void parse_channel(const Node& keys_node, BoneChannel& ch) {
    if (!keys_node.is_sequence()) return;
    for (const auto& kn : keys_node.as_seq()) {
        BoneClipKey k;
        // All-zero, not (0,0,0,1): a written location/scale channel is only ever 3 floats, and
        // set_key() zero-pads their unused 4th component the same way -- a w=1 default here
        // (the natural "identity quaternion" value) would round-trip-mismatch those two channels
        // even though a written rotation channel always supplies its own 4th component anyway.
        k.value = glm::vec4(0.0f);
        k.time = get_float(kn, "time", 0.0f);
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
        ch.keys.push_back(k);
    }
    std::sort(ch.keys.begin(), ch.keys.end(), [](const BoneClipKey& a, const BoneClipKey& b) { return a.time < b.time; });
}

Node channel_node(const BoneChannel& ch, int components) {
    Node ks = Node::sequence();
    for (const auto& k : ch.keys) {
        Node kn = Node::mapping();
        kn["time"] = make_float(k.time);
        Node v = Node::sequence();
        for (int i = 0; i < components; ++i) v.as_seq().push_back(make_float(k.value[i]));
        kn["value"] = v;
        if (!k.easing.empty()) kn["easing"] = Node(k.easing);
        ks.as_seq().push_back(kn);
    }
    return ks;
}

} // namespace

BoneClipModel BoneClipModel::from_node(const Node& root) {
    BoneClipModel m;
    if (!root.is_mapping() || !root.contains("clip")) return m;
    const Node& c = root.at("clip");
    m.name = get_string(c, "name", m.name);
    m.wrap = get_string(c, "wrap", m.wrap);

    float max_t = 0.0f;
    if (c.contains("bones") && c.at("bones").is_sequence()) {
        for (const auto& bn : c.at("bones").as_seq()) {
            BoneClipTrack t;
            t.bone = get_string(bn, "bone", "");
            if (bn.contains("location")) parse_channel(bn.at("location"), t.location);
            if (bn.contains("rotation")) parse_channel(bn.at("rotation"), t.rotation);
            if (bn.contains("scale")) parse_channel(bn.at("scale"), t.scale);
            for (const BoneChannel* ch : {&t.location, &t.rotation, &t.scale}) {
                for (const auto& k : ch->keys) max_t = std::max(max_t, k.time);
            }
            m.bones.push_back(std::move(t));
        }
    }
    if (c.contains("events") && c.at("events").is_sequence()) {
        for (const auto& en : c.at("events").as_seq()) {
            if (!en.is_mapping()) continue;
            BoneClipEvent e;
            e.time = get_float(en, "time", 0.0f);
            e.name = get_string(en, "name", "");
            e.string_value = get_string(en, "string", "");
            e.float_value = get_float(en, "float", 0.0f);
            m.events.push_back(std::move(e));
        }
        m.sort_events_();
    }
    if (c.contains("root_motion") && c.at("root_motion").is_mapping()) {
        const Node& rm = c.at("root_motion");
        m.root_motion.bone = get_string(rm, "bone", "");
        m.root_motion.translation = get_string(rm, "translation", "xy");
        m.root_motion.yaw = get_string(rm, "rotation", "none") == "yaw";
    }
    m.length = c.contains("length") ? get_float(c, "length", 1.0f) : std::max(max_t, 1.0f);
    return m;
}

Node BoneClipModel::to_node() const {
    Node c = Node::mapping();
    c["name"] = Node(name);
    c["wrap"] = Node(wrap);
    c["length"] = make_float(length);
    if (!root_motion.bone.empty()) {
        Node rm = Node::mapping();
        rm["bone"] = Node(root_motion.bone);
        rm["translation"] = Node(root_motion.translation);
        rm["rotation"] = Node(std::string(root_motion.yaw ? "yaw" : "none"));
        c["root_motion"] = rm;
    }
    if (!events.empty()) {
        Node es = Node::sequence();
        for (const auto& e : events) {
            Node en = Node::mapping();
            en["time"] = make_float(e.time);
            en["name"] = Node(e.name);
            if (!e.string_value.empty()) en["string"] = Node(e.string_value);
            if (e.float_value != 0.0f) en["float"] = make_float(e.float_value);
            es.as_seq().push_back(en);
        }
        c["events"] = es;
    }
    Node bs = Node::sequence();
    for (const auto& t : bones) {
        Node bn = Node::mapping();
        bn["bone"] = Node(t.bone);
        if (!t.location.keys.empty()) bn["location"] = channel_node(t.location, 3);
        if (!t.rotation.keys.empty()) bn["rotation"] = channel_node(t.rotation, 4);
        if (!t.scale.keys.empty()) bn["scale"] = channel_node(t.scale, 3);
        bs.as_seq().push_back(bn);
    }
    c["bones"] = bs;
    Node root = Node::mapping();
    root["clip"] = c;
    return root;
}

BoneClipTrack* BoneClipModel::find_bone_track(const std::string& bone) {
    for (auto& t : bones) if (t.bone == bone) return &t;
    return nullptr;
}

void BoneClipModel::set_key(const std::string& bone, const std::string& channel, float time, glm::vec4 value) {
    BoneClipTrack* t = find_bone_track(bone);
    if (!t) {
        bones.push_back(BoneClipTrack{bone, {}, {}, {}});
        t = &bones.back();
    }
    BoneChannel& ch = channel == "location" ? t->location : channel == "rotation" ? t->rotation : t->scale;
    for (int c = bone_channel_components(channel); c < 4; ++c) value[c] = channel == "rotation" ? 1.0f * (c == 3) : 0.0f;
    for (auto& k : ch.keys) {
        if (std::abs(k.time - time) <= kTimeEps) { k.value = value; return; }
    }
    ch.keys.push_back({time, value, ""});
    std::sort(ch.keys.begin(), ch.keys.end(), [](const BoneClipKey& a, const BoneClipKey& b) { return a.time < b.time; });
}

std::vector<float> BoneClipModel::key_times(const std::string& bone, const std::string& channel) const {
    std::vector<float> out;
    const BoneClipTrack* t = find_bone_track(bone);
    if (!t) return out;
    const BoneChannel& ch = channel == "location" ? t->location : channel == "rotation" ? t->rotation : t->scale;
    for (const auto& k : ch.keys) out.push_back(k.time);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<float> BoneClipModel::key_times(const std::string& bone) const {
    std::vector<float> out;
    const BoneClipTrack* t = find_bone_track(bone);
    if (!t) return out;
    for (const BoneChannel* ch : {&t->location, &t->rotation, &t->scale}) {
        for (const auto& k : ch->keys) {
            bool dup = false;
            for (float x : out) dup |= std::abs(x - k.time) <= kTimeEps;
            if (!dup) out.push_back(k.time);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<float> BoneClipModel::key_times() const {
    std::vector<float> out;
    for (const auto& t : bones) {
        for (const BoneChannel* ch : {&t.location, &t.rotation, &t.scale}) {
            for (const auto& k : ch->keys) {
                bool dup = false;
                for (float x : out) dup |= std::abs(x - k.time) <= kTimeEps;
                if (!dup) out.push_back(k.time);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool BoneClipModel::BoneKeyRef::operator<(const BoneKeyRef& o) const {
    if (bone != o.bone) return bone < o.bone;
    if (std::abs(time - o.time) > kTimeEps) return time < o.time;
    return channel < o.channel;
}

size_t BoneClipModel::set_easing(const std::set<BoneKeyRef>& refs, const std::string& easing) {
    size_t n = 0;
    for (auto& t : bones) {
        for (BoneChannel* ch : {&t.location, &t.rotation, &t.scale}) {
            const std::string chan = ch == &t.location ? "location" : ch == &t.rotation ? "rotation" : "scale";
            for (auto& k : ch->keys) {
                if (!covered(refs, t.bone, chan, k.time)) continue;
                k.easing = easing == "linear" ? std::string() : easing;
                ++n;
            }
        }
    }
    return n;
}

std::string BoneClipModel::easing_of(const std::set<BoneKeyRef>& refs) const {
    std::string e;
    bool any = false;
    for (const auto& t : bones) {
        for (const BoneChannel* ch : {&t.location, &t.rotation, &t.scale}) {
            const std::string chan = ch == &t.location ? "location" : ch == &t.rotation ? "rotation" : "scale";
            for (const auto& k : ch->keys) {
                if (!covered(refs, t.bone, chan, k.time)) continue;
                const std::string ke = k.easing.empty() ? "linear" : k.easing;
                if (any && ke != e) return "mixed";
                e = ke;
                any = true;
            }
        }
    }
    return e;
}

float BoneClipModel::neighbour_key(float t, int dir) const {
    float best = t;
    bool found = false;
    for (float k : key_times()) {
        if (dir > 0 && k > t + kTimeEps && (!found || k < best)) { best = k; found = true; }
        if (dir < 0 && k < t - kTimeEps && (!found || k > best)) { best = k; found = true; }
    }
    return best;
}

size_t BoneClipModel::delete_keys(const std::set<BoneKeyRef>& refs) {
    size_t n = 0;
    for (auto& t : bones) {
        for (BoneChannel* ch : {&t.location, &t.rotation, &t.scale}) {
            const std::string chan = ch == &t.location ? "location" : ch == &t.rotation ? "rotation" : "scale";
            const size_t before = ch->keys.size();
            ch->keys.erase(std::remove_if(ch->keys.begin(), ch->keys.end(),
                                          [&](const BoneClipKey& k) { return covered(refs, t.bone, chan, k.time); }),
                           ch->keys.end());
            n += before - ch->keys.size();
        }
    }
    bones.erase(std::remove_if(bones.begin(), bones.end(),
                               [](const BoneClipTrack& t) { return t.location.keys.empty() && t.rotation.keys.empty() && t.scale.keys.empty(); }),
                bones.end());
    return n;
}

std::set<BoneClipModel::BoneKeyRef> BoneClipModel::move_keys(const std::set<BoneKeyRef>& refs, float dt) {
    std::set<BoneKeyRef> moved;
    for (auto& t : bones) {
        for (BoneChannel* ch : {&t.location, &t.rotation, &t.scale}) {
            const std::string chan = ch == &t.location ? "location" : ch == &t.rotation ? "rotation" : "scale";
            std::vector<BoneClipKey> keep, shifted;
            for (const auto& k : ch->keys) {
                if (covered(refs, t.bone, chan, k.time)) {
                    BoneClipKey s = k;
                    s.time = std::max(0.0f, k.time + dt);
                    shifted.push_back(s);
                    moved.insert({t.bone, s.time, refs.count({t.bone, k.time, ""}) ? std::string() : chan});
                } else {
                    keep.push_back(k);
                }
            }
            if (shifted.empty()) continue;
            for (const auto& s : shifted) {
                keep.erase(std::remove_if(keep.begin(), keep.end(),
                                          [&](const BoneClipKey& k) { return std::abs(k.time - s.time) <= kTimeEps; }),
                           keep.end());
            }
            keep.insert(keep.end(), shifted.begin(), shifted.end());
            std::sort(keep.begin(), keep.end(), [](const BoneClipKey& a, const BoneClipKey& b) { return a.time < b.time; });
            ch->keys = std::move(keep);
        }
    }
    return moved;
}

void BoneClipModel::sort_events_() {
    std::stable_sort(events.begin(), events.end(), [](const BoneClipEvent& a, const BoneClipEvent& b) { return a.time < b.time; });
}

size_t BoneClipModel::add_event(float time, const std::string& event_name) {
    time = std::max(0.0f, time);
    BoneClipEvent e;
    e.time = time;
    e.name = event_name;
    size_t idx = events.size();
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].time > time) { idx = i; break; }
    }
    events.insert(events.begin() + static_cast<std::ptrdiff_t>(idx), std::move(e));
    return idx;
}

int BoneClipModel::move_event(size_t index, float time) {
    if (index >= events.size()) return -1;
    BoneClipEvent e = events[index];
    e.time = std::max(0.0f, time);
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(index));
    size_t idx = events.size();
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].time > e.time) { idx = i; break; }
    }
    events.insert(events.begin() + static_cast<std::ptrdiff_t>(idx), e);
    return static_cast<int>(idx);
}

bool BoneClipModel::delete_event(size_t index) {
    if (index >= events.size()) return false;
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

} // namespace editor
} // namespace toy
