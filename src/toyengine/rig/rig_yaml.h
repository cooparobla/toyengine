/**
 * @file rig_yaml.h
 * @brief Wires RigAsset and Rig into the asset system and SceneLoader.
 */

#ifndef TOYENGINE_RIG_RIG_YAML_H
#define TOYENGINE_RIG_RIG_YAML_H

#include <coopa/asset/asset_manager.h>
#include <toyengine/rig/bone_attachment.h>
#include <toyengine/rig/ik_components.h>
#include <toyengine/rig/rig_component.h>
#include <toyengine/rig/rig_loader.h>
#include <toyengine/rig/skeletal_animator.h>
#include <toyengine/rig/skeletal_clip_loader.h>
#include <coopa/scene/scene_loader.h>

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace toy {
namespace rig {

/** @brief Registers the "TwoBoneIK"/"ChainIK"/"LookAtIK" rig-space component parsers — see its definition below. */
inline void register_rig_ik_components();

/**
 * @brief Registers the RigAsset/SkeletalClip asset loaders and the "Rig"/"SkeletalAnimator"
 *        scene component parsers against the given AssetManager.
 *
 * Call once at application startup, alongside register_animation_components() and any
 * other assets.register_loader<T>() calls. The registered parsers' closures capture
 * `assets` by reference — call coopa::scene::SceneLoader::clear_component_parsers() before
 * `assets` is destroyed to avoid a dangling capture surviving into a later
 * SceneLoader::load() call.
 *
 * YAML:
 * @code
 * - type: Rig
 *   rig: rigs/mannequin.yaml
 * - type: SkeletalAnimator
 *   auto_play: idle
 *   default_crossfade: 0.15
 *   apply_root_motion: false
 *   states:
 *     - {name: idle, clip: animations/mannequin/idle.yaml}
 *     - {name: walk, clip: animations/mannequin/walk.yaml, wrap: loop, speed: 1.0}
 * @endcode
 *
 * @param assets The AssetManager the `rig:`/`clip:` paths load through.
 */
inline void register_rig_components(coopa::asset::AssetManager& assets) {
    assets.register_loader<RigAsset>(std::make_unique<RigAssetLoader>());
    assets.register_loader<SkeletalClip>(std::make_unique<SkeletalClipLoader>());

    coopa::scene::SceneLoader::register_component_parser(
        "Rig",
        [&assets](const fkyaml::node& node, coopa::scene::SceneObject& obj,
                 const coopa::scene::SceneLoader::ParseContext& ctx) {
            auto* rig = obj.add_component<Rig>();
            if (node.contains("rig")) {
                rig->rig_path = node.at("rig").get_value<std::string>();
                rig->asset = assets.load_async<RigAsset>(rig->rig_path, ctx.base_dir());
            }
        });

    coopa::scene::SceneLoader::register_component_parser(
        "SkeletalAnimator",
        [&assets](const fkyaml::node& node, coopa::scene::SceneObject& obj,
                 const coopa::scene::SceneLoader::ParseContext& ctx) {
            auto* animator = obj.add_component<SkeletalAnimator>();

            if (node.contains("auto_play")) animator->auto_play = node.at("auto_play").get_value<std::string>();
            if (node.contains("speed")) animator->set_speed(node.at("speed").get_value<float>());
            if (node.contains("default_crossfade")) animator->default_crossfade = node.at("default_crossfade").get_value<float>();
            if (node.contains("apply_root_motion")) animator->apply_root_motion = node.at("apply_root_motion").get_value<bool>();

            if (!node.contains("states")) return;
            for (const auto& state_node : node.at("states")) {
                if (!state_node.contains("name") || !state_node.contains("clip")) continue;
                const std::string name = state_node.at("name").get_value<std::string>();
                const std::string clip_path = state_node.at("clip").get_value<std::string>();

                auto handle = assets.load_async<SkeletalClip>(clip_path, ctx.base_dir());
                SkeletalAnimatorState* state = animator->add_state(name, std::move(handle));

                if (state_node.contains("wrap")) {
                    state->has_wrap_override = true;
                    state->wrap_override = toy::anim::parse_wrap_mode(state_node.at("wrap").get_value<std::string>());
                }
                if (state_node.contains("speed")) state->speed = state_node.at("speed").get_value<float>();
            }
        });

    coopa::scene::SceneLoader::register_component_parser(
        "BoneAttachment",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* ba = obj.add_component<BoneAttachment>();
            if (node.contains("bone")) ba->bone = node.at("bone").get_value<std::string>();
            if (node.contains("rig")) ba->rig_path = node.at("rig").get_value<std::string>();
            if (node.contains("at")) {
                ba->at = node.at("at").get_value<std::string>() == "tail" ? BoneAttachment::Anchor::Tail : BoneAttachment::Anchor::Head;
            }
        });

    register_rig_ik_components();
}

/**
 * @brief Registers the "TwoBoneIK", "ChainIK" and "LookAtIK" rig-space scene component
 *        parsers (see ik_components.h for the keys). Called by register_rig_components();
 *        the solver system itself is installed separately (install_ik_system(), ik_system.h).
 */
inline void register_rig_ik_components() {
    auto read_str = [](const fkyaml::node& n, const char* key, std::string& out) {
        if (n.contains(key)) out = n.at(key).get_value<std::string>();
    };
    auto read_float = [](const fkyaml::node& n, const char* key, float& out) {
        if (n.contains(key)) out = n.at(key).get_value<float>();
    };
    auto read_bool = [](const fkyaml::node& n, const char* key, bool& out) {
        if (n.contains(key)) out = n.at(key).get_value<bool>();
    };
    auto read_int = [](const fkyaml::node& n, const char* key, int& out) {
        if (n.contains(key)) out = static_cast<int>(n.at(key).get_value<int64_t>());
    };
    auto read_vec3 = [](const fkyaml::node& n, const char* key, glm::vec3& out) {
        if (n.contains(key)) out = parse_rig_vec3(n.at(key));
    };

    coopa::scene::SceneLoader::register_component_parser(
        "TwoBoneIK",
        [=](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* ik = obj.add_component<TwoBoneIK>();
            read_str(node, "rig", ik->rig_path);
            read_str(node, "upper", ik->upper);
            read_str(node, "lower", ik->lower);
            read_str(node, "end", ik->end);
            read_str(node, "target", ik->target);
            read_str(node, "pole", ik->pole);
            read_float(node, "weight", ik->weight);
            read_float(node, "soft_limit", ik->soft_limit);
            read_bool(node, "match_target_rotation", ik->match_target_rotation);
            read_int(node, "priority", ik->priority);
        });

    coopa::scene::SceneLoader::register_component_parser(
        "ChainIK",
        [=](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* ik = obj.add_component<ChainIK>();
            read_str(node, "rig", ik->rig_path);
            read_str(node, "root", ik->root);
            read_str(node, "tip", ik->tip);
            read_str(node, "target", ik->target);
            read_float(node, "weight", ik->weight);
            read_int(node, "iterations", ik->iterations);
            read_float(node, "tolerance", ik->tolerance);
            read_bool(node, "match_target_rotation", ik->match_target_rotation);
            read_int(node, "priority", ik->priority);
        });

    coopa::scene::SceneLoader::register_component_parser(
        "LookAtIK",
        [=](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* ik = obj.add_component<LookAtIK>();
            read_str(node, "rig", ik->rig_path);
            read_str(node, "bone", ik->bone);
            read_str(node, "target", ik->target);
            read_vec3(node, "forward_axis", ik->forward_axis);
            read_vec3(node, "up_axis", ik->up_axis);
            read_float(node, "max_angle", ik->max_angle);
            read_float(node, "weight", ik->weight);
            read_float(node, "smoothing", ik->smoothing);
            read_int(node, "priority", ik->priority);
        });
}

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_RIG_YAML_H
