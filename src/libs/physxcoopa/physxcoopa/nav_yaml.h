/**
 * @file nav_yaml.h
 * @brief register_nav_components() -- YAML parsers for NavAgent, NavModifier, NavVolume and
 *        NavLink. Called by register_physics_components() (physx_yaml.h), so a physics user
 *        gets them without asking; install_nav_system() (system/nav_system.h) is what makes
 *        them do anything.
 */

#ifndef PHYSXCOOPA_NAV_YAML_H
#define PHYSXCOOPA_NAV_YAML_H

#include <physxcoopa/components/nav_agent.h>
#include <physxcoopa/components/nav_modifier.h>
#include <physxcoopa/components/nav_volume.h>
#include <physxcoopa/components/nav_link.h>
#include <physxcoopa/nav/nav_settings.h>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {

namespace detail {

inline glm::vec3 nav_vec3_(const fkyaml::node& node, const char* key, const glm::vec3& fallback) {
    return node.contains(key) ? nav::detail::read_xyz_(node.at(key), fallback) : fallback;
}

inline float nav_float_(const fkyaml::node& node, const char* key, float fallback) {
    return nav::detail::read_float_(node, key, fallback);
}

inline std::string nav_area_(const fkyaml::node& node, const std::string& fallback) {
    if (!node.contains("area")) return fallback;
    const auto& a = node.at("area");
    if (a.is_integer()) return std::to_string(a.get_value<int64_t>());
    return a.get_value<std::string>();
}

} // namespace detail

/**
 * @brief Registers the navigation component parsers.
 *
 * @code
 * - type: NavAgent
 *   agent_type: Humanoid        # profile from `navigation: agents:`
 *   speed: 3.5
 *   acceleration: 12
 *   angular_speed: 720          # deg/s
 *   stopping_distance: 0.15
 *   slowdown_distance: 1.0
 *   radius: 0.4                 # separation radius (default: the profile's)
 *   separation_weight: 2.0      # 0 = no crowd avoidance
 *   base_offset: 0.0            # origin height above the surface
 *   update_rotation: true
 *   forward: Y                  # local axis facing travel: Y, -Y, X, -X
 *   avoid_areas: [Water]
 *   heuristic_weight: 1.1
 *   destination: { x: 4, y: 2, z: 0 }   # A* to a point...
 *   destination_object: Player          # ...or chase an object by A*...
 *   flow_target: Player                 # ...or follow the object's shared flow field
 * - type: NavModifier
 *   area: Grass                 # or walkable: false / ignore: true
 *   carve: true                 # dynamic bodies: carve while asleep
 *   apply_to_children: true
 * - type: NavVolume
 *   size: { x: 4, y: 4, z: 2 }
 *   center: { x: 0, y: 0, z: 0 }
 *   area: Water                 # NotWalkable cuts a hole
 * - type: NavLink
 *   start: { x: 0, y: 0, z: 0 }
 *   end: { x: 0, y: 3, z: -2 }
 *   bidirectional: false
 *   cost: 1.0
 *   snap_radius: 1.0
 * @endcode
 */
inline void register_nav_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("NavAgent",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* a = obj.add_component<components::NavAgentComponent>();
            if (node.contains("agent_type")) a->agent_type = node.at("agent_type").get_value<std::string>();
            a->speed = detail::nav_float_(node, "speed", a->speed);
            a->acceleration = detail::nav_float_(node, "acceleration", a->acceleration);
            a->angular_speed = detail::nav_float_(node, "angular_speed", a->angular_speed);
            a->stopping_distance = detail::nav_float_(node, "stopping_distance", a->stopping_distance);
            a->slowdown_distance = detail::nav_float_(node, "slowdown_distance", a->slowdown_distance);
            a->radius = detail::nav_float_(node, "radius", a->radius);
            a->separation_weight = detail::nav_float_(node, "separation_weight", a->separation_weight);
            a->base_offset = detail::nav_float_(node, "base_offset", a->base_offset);
            a->heuristic_weight = detail::nav_float_(node, "heuristic_weight", a->heuristic_weight);
            if (node.contains("update_rotation")) a->update_rotation = node.at("update_rotation").get_value<bool>();
            if (node.contains("forward")) a->forward = node.at("forward").get_value<std::string>();
            if (node.contains("avoid_areas")) {
                for (const auto& n : node.at("avoid_areas")) a->avoid_areas.push_back(n.get_value<std::string>());
            }
            if (node.contains("destination")) a->set_destination(nav::detail::read_xyz_(node.at("destination"), glm::vec3(0.0f)));
            if (node.contains("destination_object")) a->set_destination_object(node.at("destination_object").get_value<std::string>());
            if (node.contains("flow_target")) a->set_flow_target(node.at("flow_target").get_value<std::string>());
        });

    SceneLoader::register_component_parser("NavModifier",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* m = obj.add_component<components::NavModifierComponent>();
            m->area = detail::nav_area_(node, m->area);
            if (node.contains("walkable")) m->walkable = node.at("walkable").get_value<bool>();
            if (node.contains("ignore")) m->ignore = node.at("ignore").get_value<bool>();
            if (node.contains("carve")) m->carve = node.at("carve").get_value<bool>();
            if (node.contains("apply_to_children")) m->apply_to_children = node.at("apply_to_children").get_value<bool>();
        });

    SceneLoader::register_component_parser("NavVolume",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* v = obj.add_component<components::NavVolumeComponent>();
            v->size = detail::nav_vec3_(node, "size", v->size);
            v->center = detail::nav_vec3_(node, "center", v->center);
            v->area = detail::nav_area_(node, v->area);
        });

    SceneLoader::register_component_parser("NavLink",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* l = obj.add_component<components::NavLinkComponent>();
            l->start = detail::nav_vec3_(node, "start", l->start);
            l->end = detail::nav_vec3_(node, "end", l->end);
            if (node.contains("bidirectional")) l->bidirectional = node.at("bidirectional").get_value<bool>();
            l->cost = detail::nav_float_(node, "cost", l->cost);
            l->snap_radius = detail::nav_float_(node, "snap_radius", l->snap_radius);
            l->area = detail::nav_area_(node, l->area);
        });
}

} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_YAML_H
