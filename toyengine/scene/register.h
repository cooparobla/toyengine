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

            auto parse_vec3 = [](const fkyaml::node& n, const glm::vec3& fallback) {
                glm::vec3 v = fallback;
                if (n.contains("x")) v.x = n.at("x").get_value<float>();
                if (n.contains("y")) v.y = n.at("y").get_value<float>();
                if (n.contains("z")) v.z = n.at("z").get_value<float>();
                return v;
            };

            if (node.contains("mode")) {
                std::string m = node.at("mode").get_value<std::string>();
                cc->mode = (m == "fly" || m == "Fly") ? CameraControlMode::Fly : CameraControlMode::Orbit;
            }

            if (node.contains("tracker")) {
                cc->tracker = node.at("tracker").get_value<std::string>();
            }
            if (node.contains("target")) {
                cc->target = parse_vec3(node.at("target"), cc->target);
            }
            if (node.contains("target_offset")) {
                cc->target_offset = parse_vec3(node.at("target_offset"), cc->target_offset);
            }
            if (node.contains("follow_smoothing")) {
                cc->follow_smoothing = node.at("follow_smoothing").get_value<float>();
            }
            if (node.contains("movement_smoothing")) {
                cc->movement_smoothing = node.at("movement_smoothing").get_value<float>();
            }

            if (node.contains("distance")) cc->distance = node.at("distance").get_value<float>();
            if (node.contains("yaw_deg"))   cc->yaw_deg   = node.at("yaw_deg").get_value<float>();
            if (node.contains("pitch_deg")) cc->pitch_deg = node.at("pitch_deg").get_value<float>();

            if (node.contains("min_pitch_deg")) cc->min_pitch_deg = node.at("min_pitch_deg").get_value<float>();
            if (node.contains("max_pitch_deg")) cc->max_pitch_deg = node.at("max_pitch_deg").get_value<float>();
            if (node.contains("min_distance"))  cc->min_distance  = node.at("min_distance").get_value<float>();
            if (node.contains("max_distance"))  cc->max_distance  = node.at("max_distance").get_value<float>();

            if (node.contains("mouse_sensitivity")) {
                cc->mouse_sensitivity = node.at("mouse_sensitivity").get_value<float>();
            }
            if (node.contains("invert_x")) cc->invert_x = node.at("invert_x").get_value<bool>();
            if (node.contains("invert_y")) cc->invert_y = node.at("invert_y").get_value<bool>();
            if (node.contains("zoom_speed")) cc->zoom_speed = node.at("zoom_speed").get_value<float>();
            if (node.contains("capture_cursor")) {
                cc->capture_cursor = node.at("capture_cursor").get_value<bool>();
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
