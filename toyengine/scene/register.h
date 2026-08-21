/**
 * @file register.h
 * @brief Registers toyengine's own scene components as SceneLoader parsers.
 *
 * Mirrors gfxcoopa/engine/components/register.h's pattern: unlike
 * MeshRenderer et al., CameraController needs no GPU handles, so its parser
 * is a free function taking no captured dependencies.
 */

#ifndef TOYENGINE_SCENE_REGISTER_H
#define TOYENGINE_SCENE_REGISTER_H

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <fkYAML/node.hpp>

#include <toyengine/scene/camera_controller.h>

namespace toy {
namespace scene {

/**
 * @brief Registers the "CameraController" component parser with SceneLoader.
 *
 * Call once at startup, before the first SceneLoader::load() that uses it.
 */
inline void register_scene_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("CameraController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cc = obj.add_component<CameraController>();

            if (node.contains("mode")) {
                std::string m = node.at("mode").get_value<std::string>();
                cc->mode = (m == "fly" || m == "Fly") ? CameraControlMode::Fly : CameraControlMode::Orbit;
            }
            if (node.contains("target")) {
                const auto& tgt = node.at("target");
                cc->target = glm::vec3(
                    tgt.contains("x") ? tgt.at("x").get_value<float>() : 0.0f,
                    tgt.contains("y") ? tgt.at("y").get_value<float>() : 0.0f,
                    tgt.contains("z") ? tgt.at("z").get_value<float>() : 0.0f);
            }
            if (node.contains("auto_rotate_deg_per_sec")) {
                cc->auto_rotate_deg_per_sec = node.at("auto_rotate_deg_per_sec").get_value<float>();
            }
            if (node.contains("move_speed")) {
                cc->move_speed = node.at("move_speed").get_value<float>();
            }
            if (node.contains("look_speed_deg_per_sec")) {
                cc->look_speed_deg_per_sec = node.at("look_speed_deg_per_sec").get_value<float>();
            }
        });
}

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_REGISTER_H
