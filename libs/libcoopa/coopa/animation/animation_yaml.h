/**
 * @file animation_yaml.h
 * @brief Wires AnimationClip and Animator into the asset system and SceneLoader.
 */

#ifndef COOPA_ANIMATION_ANIMATION_YAML_H
#define COOPA_ANIMATION_ANIMATION_YAML_H

#include <coopa/animation/animation_clip_loader.h>
#include <coopa/animation/animator.h>
#include <coopa/animation/ik_components.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>

#include <cstdint>
#include <memory>
#include <string>

namespace coopa {
namespace anim {

/**
 * @brief Registers the "TwoBoneIK", "ChainIK" and "LookAtIK" scene component parsers (see
 *        ik_components.h for the keys). Called by register_animation_components(); the solver
 *        system itself is installed separately (install_ik_system(), ik_system.h).
 */
inline void register_ik_components() {
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
        if (!n.contains(key)) return;
        float v[4];
        parse_value4(n.at(key), v);
        out = glm::vec3(v[0], v[1], v[2]);
    };

    coopa::scene::SceneLoader::register_component_parser(
        "TwoBoneIK",
        [=](const fkyaml::node& node, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            auto* ik = obj.add_component<TwoBoneIK>();
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

/**
 * @brief Registers the AnimationClip asset loader and the "Animator" scene
 *        component parser against the given AssetManager.
 *
 * Call once at application startup, alongside any other
 * assets.register_loader<T>() calls (see coopa/asset/README.md's pattern).
 * The registered parser's closure captures `assets` by reference — call
 * coopa::scene::SceneLoader::clear_component_parsers() before `assets` is
 * destroyed to avoid a dangling capture surviving into a later
 * SceneLoader::load() call.
 *
 * The component tag is "Animator". Like every non-Transform component, it is
 * registered from outside libcoopa rather than built into SceneLoader — see
 * coopa/animation/README.md.
 *
 * @param assets The AssetManager clip states' `clip:` paths load through.
 */
inline void register_animation_components(coopa::asset::AssetManager& assets) {
    assets.register_loader<AnimationClip>(std::make_unique<AnimationClipLoader>());

    coopa::scene::SceneLoader::register_component_parser(
        "Animator",
        [&assets](const fkyaml::node& node, coopa::scene::SceneObject& obj,
                 const coopa::scene::SceneLoader::ParseContext& ctx) {
            auto* animator = obj.add_component<Animator>();

            if (node.contains("auto_play")) {
                animator->auto_play = node.at("auto_play").get_value<std::string>();
            }
            if (node.contains("speed")) {
                animator->set_speed(node.at("speed").get_value<float>());
            }
            if (node.contains("default_crossfade")) {
                animator->default_crossfade = node.at("default_crossfade").get_value<float>();
            }
            if (node.contains("apply_root_motion")) {
                animator->apply_root_motion = node.at("apply_root_motion").get_value<bool>();
            }

            if (!node.contains("states")) return;
            for (const auto& state_node : node.at("states")) {
                if (!state_node.contains("name") || !state_node.contains("clip")) continue;
                std::string name = state_node.at("name").get_value<std::string>();
                std::string clip_path = state_node.at("clip").get_value<std::string>();

                // Unresolved relative path + scene_dir as base_dir, matching
                // the established loader-registration pattern (e.g.
                // gfxcoopa's MeshRenderer/material texture loading) rather
                // than pre-resolving via ParseContext::resolve() — this lets
                // AssetManager's own AssetSource search roots participate too.
                auto handle = assets.load_async<AnimationClip>(clip_path, ctx.base_dir());
                AnimatorState* state = animator->add_state(name, std::move(handle));

                if (state_node.contains("wrap")) {
                    state->has_wrap_override = true;
                    state->wrap_override = parse_wrap_mode(state_node.at("wrap").get_value<std::string>());
                }
                if (state_node.contains("speed")) {
                    state->speed = state_node.at("speed").get_value<float>();
                }
            }
        });

    register_ik_components();
}

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ANIMATION_YAML_H
