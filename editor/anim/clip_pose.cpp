#include "editor/anim/clip_pose.h"

namespace toy {
namespace editor {

coopa::scene::SceneObject* resolve_rig_path(coopa::scene::SceneObject* root, const std::string& path) {
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

float wrap_clip_time(const coopa::anim::AnimationClip& clip, float t) {
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

int apply_clip_pose(const coopa::anim::AnimationClip& clip, float t, coopa::scene::SceneObject* root) {
    auto& reg = coopa::anim::AnimatedPropertyRegistry::instance();
    int applied = 0;
    coopa::scene::SceneObject* root_motion_obj =
        clip.root_motion.enabled() ? resolve_rig_path(root, clip.root_motion.object) : nullptr;
    const uint8_t root_channels = clip.root_motion.translation == coopa::anim::RootMotionTranslation::XYZ ? 0x07
                                : clip.root_motion.translation == coopa::anim::RootMotionTranslation::XY  ? 0x03 : 0x00;
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
        const uint8_t held = (obj == root_motion_obj && track.component_type == "Transform" && prop->name == "position")
                                 ? static_cast<uint8_t>(mask & root_channels) : uint8_t{0};
        if (mask == static_cast<uint8_t>((1u << prop->component_count) - 1u)) {
            track.curve.sample(t, prop->component_count, val);
            if (held) {
                float start[4] = {0, 0, 0, 1};
                track.curve.sample(0.0f, prop->component_count, start);
                for (int c = 0; c < prop->component_count; ++c) if ((held >> c) & 1) val[c] = start[c];
            }
        } else {
            // A channel suffix: the curve's channels map onto the masked ones, in order.
            float sampled[4] = {0, 0, 0, 0}, start[4] = {0, 0, 0, 0};
            track.curve.sample(t, 4, sampled);
            if (held) track.curve.sample(0.0f, 4, start);
            int k = 0;
            for (int c = 0; c < prop->component_count; ++c) {
                if (!((mask >> c) & 1)) { val[c] = cur[c]; continue; }
                val[c] = (held >> c) & 1 ? start[k] : sampled[k];
                ++k;
            }
        }
        prop->set(target, val);
        ++applied;
    }
    return applied;
}

} // namespace editor
} // namespace toy
