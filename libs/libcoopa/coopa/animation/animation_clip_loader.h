/**
 * @file animation_clip_loader.h
 * @brief Parses AnimationClip from YAML and adapts that into an asset loader.
 */

#ifndef COOPA_ANIMATION_ANIMATION_CLIP_LOADER_H
#define COOPA_ANIMATION_ANIMATION_CLIP_LOADER_H

#include <coopa/animation/animation_clip.h>
#include <coopa/asset/asset_loader.h>
#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>
#include <glm/glm.hpp>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace coopa {
namespace anim {

/**
 * @brief Reads a scalar, {x,y,z,w}/{r,g,b,a} mapping, or [a,b,c,d] sequence
 *        into a 4-float array, pre-initialized to the glm-idiomatic xyzw/rgba
 *        identity {0,0,0,1} — matching Keyframe::value's own default, so a
 *        color key omitting `a` gets 1.0 and a vec2 key gets 0 in z.
 *
 * @param node The YAML node to read.
 * @param out Destination; out[0..3] are all written.
 */
inline void parse_value4(const fkyaml::node& node, float out[4]) {
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
    if (node.is_scalar()) {
        out[0] = node.get_value<float>();
    } else if (node.is_mapping()) {
        if (node.contains("x")) out[0] = node.at("x").get_value<float>();
        if (node.contains("y")) out[1] = node.at("y").get_value<float>();
        if (node.contains("z")) out[2] = node.at("z").get_value<float>();
        if (node.contains("w")) out[3] = node.at("w").get_value<float>();
        if (node.contains("r")) out[0] = node.at("r").get_value<float>();
        if (node.contains("g")) out[1] = node.at("g").get_value<float>();
        if (node.contains("b")) out[2] = node.at("b").get_value<float>();
        if (node.contains("a")) out[3] = node.at("a").get_value<float>();
    } else if (node.is_sequence()) {
        int i = 0;
        for (const auto& item : node) {
            if (i >= 4) break;
            out[i++] = item.get_value<float>();
        }
    }
}

/** @brief Parses one AnimationTrack from its YAML node — either a `keys:` list or a `procedural:` block. */
inline AnimationTrack parse_track(const fkyaml::node& node) {
    AnimationTrack track;
    if (node.contains("object")) track.object_path = node.at("object").get_value<std::string>();
    if (node.contains("component")) track.component_type = node.at("component").get_value<std::string>();
    if (node.contains("component_index")) track.component_index = node.at("component_index").get_value<int>();
    if (node.contains("property")) track.property = node.at("property").get_value<std::string>();

    if (node.contains("procedural")) {
        track.kind = TrackKind::Procedural;
        const auto& p = node.at("procedural");
        if (p.contains("type")) track.procedural.type = p.at("type").get_value<std::string>();
        if (p.contains("target_object")) track.procedural.target_object = p.at("target_object").get_value<std::string>();
        for (auto item : p.map_items()) {
            std::string key = item.key().get_value<std::string>();
            if (key == "type" || key == "target_object") continue;
            const fkyaml::node& value_node = item.value();
            if (value_node.is_scalar()) {
                track.procedural.params.scalars[key] = value_node.get_value<float>();
            } else {
                float v[4];
                parse_value4(value_node, v);
                track.procedural.params.vectors[key] = glm::vec4(v[0], v[1], v[2], v[3]);
            }
        }
    } else if (node.contains("keys")) {
        track.kind = TrackKind::Keyframed;
        for (const auto& key_node : node.at("keys")) {
            Keyframe kf;
            if (key_node.contains("time")) kf.time = key_node.at("time").get_value<float>();
            if (key_node.contains("value")) parse_value4(key_node.at("value"), kf.value);
            if (key_node.contains("easing")) kf.easing = parse_interpolation(key_node.at("easing").get_value<std::string>());
            track.curve.add_key(kf);
        }
        track.curve.sort_keys();
    }
    return track;
}

/**
 * @brief Parses a full AnimationClip from an already-deserialized YAML root node.
 *
 * Expects the `clip:` mapping form described in coopa/animation/README.md —
 * a flat mapping (never a `!Tag` block, per the fkYAML block-tag limitation
 * documented in coopa/scene/scene_loader.h). Shared by AnimationClipLoader
 * and tests that want to parse a clip fixture directly.
 *
 * @throws std::runtime_error if the root has no `clip:` key.
 */
inline AnimationClip parse_clip(const fkyaml::node& root) {
    if (!root.contains("clip")) {
        throw std::runtime_error("[AnimationClip] root node has no 'clip:' key");
    }
    const auto& node = root.at("clip");

    AnimationClip clip;
    if (node.contains("name")) clip.name = node.at("name").get_value<std::string>();
    if (node.contains("wrap")) clip.wrap = parse_wrap_mode(node.at("wrap").get_value<std::string>());
    if (node.contains("length")) clip.set_explicit_length(node.at("length").get_value<float>());

    if (node.contains("tracks")) {
        for (const auto& track_node : node.at("tracks")) {
            clip.tracks.push_back(parse_track(track_node));
        }
    }
    if (node.contains("events") && node.at("events").is_sequence()) {
        for (const auto& ev_node : node.at("events")) {
            if (!ev_node.is_mapping()) continue;
            AnimationEvent ev;
            if (ev_node.contains("time")) ev.time = ev_node.at("time").get_value<float>();
            if (ev_node.contains("name")) ev.name = ev_node.at("name").get_value<std::string>();
            if (ev_node.contains("string")) ev.string_value = ev_node.at("string").get_value<std::string>();
            if (ev_node.contains("float")) ev.float_value = ev_node.at("float").get_value<float>();
            clip.events.push_back(std::move(ev));
        }
        clip.sort_events();
    }
    if (node.contains("root_motion") && node.at("root_motion").is_mapping()) {
        const auto& rm = node.at("root_motion");
        if (rm.contains("object")) clip.root_motion.object = rm.at("object").get_value<std::string>();
        if (rm.contains("translation")) {
            clip.root_motion.translation = parse_root_motion_translation(rm.at("translation").get_value<std::string>());
        }
        if (rm.contains("rotation")) clip.root_motion.yaw = rm.at("rotation").get_value<std::string>() == "yaw";
    }
    return clip;
}

/**
 * @class AnimationClipLoader
 * @brief coopa::asset TypedAssetLoader for AnimationClip — a pure-CPU asset.
 *
 * decode_typed() runs entirely on the asset IO worker thread and is safe to
 * do so precisely because a clip stores only strings/floats — no registry
 * lookup, no SceneObject*, no Component* (those are resolved later, on the
 * main thread, in Animator::rebind()). finalize_typed() is a pass-through,
 * the "plain CPU data asset with nothing to upload" case IAssetLoader's own
 * doc calls out.
 */
class AnimationClipLoader : public coopa::asset::TypedAssetLoader<AnimationClip> {
public:
    std::shared_ptr<AnimationClip> decode_typed(const coopa::asset::AssetId& id,
                                                const coopa::asset::LoadContext& ctx) override {
        fkyaml::node root = coopa::yaml::load_document(ctx.resolved_path);
        return std::make_shared<AnimationClip>(parse_clip(root));
    }

    std::shared_ptr<AnimationClip> finalize_typed(std::shared_ptr<AnimationClip> decoded,
                                                  const coopa::asset::AssetId&,
                                                  const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "AnimationClip"; }
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ANIMATION_CLIP_LOADER_H
