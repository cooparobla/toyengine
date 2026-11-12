/**
 * @file skeletal_clip_loader.h
 * @brief Parses SkeletalClip from YAML and adapts that into an asset loader.
 */

#ifndef TOYENGINE_RIG_SKELETAL_CLIP_LOADER_H
#define TOYENGINE_RIG_SKELETAL_CLIP_LOADER_H

#include <toyengine/anim/animation_clip_loader.h> // parse_value4()
#include <coopa/asset/asset_loader.h>
#include <toyengine/rig/skeletal_clip.h>
#include <coopa/yaml/document.h>
#include <fkYAML/node.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace toy {
namespace rig {

/** @brief Parses one SkeletalTrack from its YAML node: a bone name plus its keyed channels. */
inline SkeletalTrack parse_skeletal_track(const fkyaml::node& node) {
    SkeletalTrack track;
    if (node.contains("bone")) track.bone = node.at("bone").get_value<std::string>();

    auto parse_curve = [](const fkyaml::node& keys_node, toy::anim::AnimationCurve& curve) {
        for (const auto& key_node : keys_node) {
            toy::anim::Keyframe kf;
            if (key_node.contains("time")) kf.time = key_node.at("time").get_value<float>();
            if (key_node.contains("value")) toy::anim::parse_value4(key_node.at("value"), kf.value);
            if (key_node.contains("easing")) kf.easing = toy::anim::parse_interpolation(key_node.at("easing").get_value<std::string>());
            curve.add_key(kf);
        }
        curve.sort_keys();
    };
    if (node.contains("location")) parse_curve(node.at("location"), track.location);
    if (node.contains("rotation")) parse_curve(node.at("rotation"), track.rotation);
    if (node.contains("scale")) parse_curve(node.at("scale"), track.scale);
    return track;
}

/**
 * @brief Parses a full SkeletalClip from an already-deserialized YAML root node.
 *
 * Expects the `clip:` mapping form: `name:`/`wrap:`/`length:`, a `bones:` sequence (each a
 * `bone:` name plus `location:`/`rotation:`/`scale:` key lists), `events:` and `root_motion:`
 * (keyed by `bone:` rather than `object:`).
 *
 * @throws std::runtime_error if the root has no `clip:` key.
 */
inline SkeletalClip parse_skeletal_clip(const fkyaml::node& root) {
    if (!root.contains("clip")) {
        throw std::runtime_error("[SkeletalClip] root node has no 'clip:' key");
    }
    const auto& node = root.at("clip");

    SkeletalClip clip;
    if (node.contains("name")) clip.name = node.at("name").get_value<std::string>();
    if (node.contains("wrap")) clip.wrap = toy::anim::parse_wrap_mode(node.at("wrap").get_value<std::string>());
    if (node.contains("length")) clip.set_explicit_length(node.at("length").get_value<float>());

    if (node.contains("bones")) {
        for (const auto& bone_node : node.at("bones")) {
            clip.bones.push_back(parse_skeletal_track(bone_node));
        }
    }
    if (node.contains("events") && node.at("events").is_sequence()) {
        for (const auto& ev_node : node.at("events")) {
            if (!ev_node.is_mapping()) continue;
            toy::anim::AnimationEvent ev;
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
        if (rm.contains("bone")) clip.root_motion.bone = rm.at("bone").get_value<std::string>();
        if (rm.contains("translation")) {
            clip.root_motion.translation = toy::anim::parse_root_motion_translation(rm.at("translation").get_value<std::string>());
        }
        if (rm.contains("rotation")) clip.root_motion.yaw = rm.at("rotation").get_value<std::string>() == "yaw";
    }
    return clip;
}

/**
 * @class SkeletalClipLoader
 * @brief coopa::asset TypedAssetLoader for SkeletalClip — a pure-CPU asset.
 *
 * decode_typed() runs entirely on the asset IO worker thread, exactly like
 * toy::anim::AnimationClipLoader: a skeletal clip stores only strings/floats — bone
 * lookup happens later, on the main thread, in SkeletalAnimator::rebind().
 */
class SkeletalClipLoader : public coopa::asset::TypedAssetLoader<SkeletalClip> {
public:
    std::shared_ptr<SkeletalClip> decode_typed(const coopa::asset::AssetId&,
                                               const coopa::asset::LoadContext& ctx) override {
        fkyaml::node root = coopa::yaml::load_document(ctx.resolved_path);
        return std::make_shared<SkeletalClip>(parse_skeletal_clip(root));
    }

    std::shared_ptr<SkeletalClip> finalize_typed(std::shared_ptr<SkeletalClip> decoded,
                                                 const coopa::asset::AssetId&,
                                                 const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "SkeletalClip"; }
};

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_SKELETAL_CLIP_LOADER_H
