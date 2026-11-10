/**
 * @file nav_settings.h
 * @brief NavSettings -- everything a `navigation:` YAML block configures: the build (cell size,
 *        agent profiles, areas, contributing physics layers) and the runtime (path budget,
 *        repath and flow-field rebuild rules, debug drawing).
 */

#ifndef PHYSXCOOPA_NAV_NAV_SETTINGS_H
#define PHYSXCOOPA_NAV_NAV_SETTINGS_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_debug.h>
#include <physxcoopa/nav/flow_field.h>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/**
 * @struct NavSettings
 * @brief Scene/project-wide navigation settings. See parse_nav_settings() for the YAML shape.
 */
struct NavSettings {
    /** @brief False skips building and agent updates entirely. */
    bool enabled = true;
    /** @brief Build even while the scene has no NavAgent (for gameplay code that only queries
     *         NavSystem::find_path()/mesh()). Off by default, so a scene that never uses
     *         navigation pays nothing for it. */
    bool build_without_agents = false;
    NavBuildSettings build;

    /** @brief Path queries run per frame (the rest wait). They run in parallel on the job engine. */
    int max_path_requests_per_frame = 64;
    /** @brief A NavAgent following a moving object re-plans at most this often, seconds. */
    float repath_interval = 0.5f;
    /** @brief ...and only once its target has moved at least this far, metres. */
    float repath_distance = 0.5f;

    /** @brief Flow fields rebuild when their target moves this far, metres. */
    float flow_rebuild_distance = 0.5f;
    /** @brief Flow-field march bound, metres (0 = the target's whole connected area). */
    float flow_max_distance = 0.0f;
    /** @brief Flow-field wall penalty (see QueryFilter::wall_penalty). */
    float flow_wall_penalty = 1.0f;
    /** @brief March each flow field only as far as the agents following it (plus a margin),
     *         rebuilding when one wanders out -- see FlowFieldSettings::required_points. */
    bool flow_bound_to_followers = true;
    /** @brief Flow field integration (see FlowFieldMode): Auto is exact for small areas and
     *         hierarchical -- exact only where the followers are -- for open worlds. */
    FlowFieldMode flow_mode = FlowFieldMode::Auto;
    uint32_t flow_exact_tile_budget = 64;
    float flow_near_radius = 12.0f;
    int flow_lookahead_tiles = 2;
    /** @brief Minimum seconds between rebuilds of one field triggered by followers leaving its
     *         exact area (they steer by the coarse layer meanwhile). */
    float flow_rebuild_interval = 0.25f;

    /** @brief Seconds between rescans of the scene's colliders, volumes and links for changes. */
    float source_scan_interval = 0.1f;

    /** @brief Crowds update in parallel above this many agents. */
    std::size_t parallel_threshold = 64;

    NavDebugFlags debug_draw = NavDebugFlags::None;
    /** @brief Flow arrows every N cells. */
    int debug_flow_stride = 2;
};

namespace detail {

inline float read_float_(const fkyaml::node& n, const char* key, float fallback) {
    if (!n.contains(key)) return fallback;
    const auto& v = n.at(key);
    if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return fallback;
}

inline glm::vec3 read_xyz_(const fkyaml::node& n, const glm::vec3& fallback) {
    glm::vec3 v = fallback;
    if (n.contains("x")) v.x = read_float_(n, "x", v.x);
    if (n.contains("y")) v.y = read_float_(n, "y", v.y);
    if (n.contains("z")) v.z = read_float_(n, "z", v.z);
    return v;
}

} // namespace detail

/**
 * @brief Parses a `navigation:` node. Every key is optional.
 *
 * @param physics_layer_names Physics layer names (PhysicsSettings::layer_names), so `layers:`
 *        may name layers instead of numbering them.
 *
 * @code
 * navigation:
 *   enabled: true
 *   build_without_agents: false  # build even with no NavAgent in the scene
 *   cell_size: 0.25            # XY resolution, metres
 *   cell_height: 0.1           # Z resolution
 *   tile_size: 32              # cells per tile side
 *   max_slope: 45              # degrees
 *   bounds: { min: { x: -50, y: -50, z: -5 }, max: { x: 50, y: 50, z: 20 } }   # default: fit colliders
 *   layers: [Default, Ground]  # physics layers that contribute (default: all)
 *   agents:
 *     - { name: Humanoid, radius: 0.4, height: 1.8, max_climb: 0.45 }
 *     - { name: Ogre, radius: 1.0, height: 3.0, max_climb: 0.6 }
 *   areas:
 *     - { name: Grass, cost: 1.5 }       # ids are assigned from 2 upward...
 *     - { name: Water, cost: 5, id: 9 }  # ...unless given
 *   rebuild_delay: 0.25
 *   max_concurrent_rebuilds: 64
 *   max_path_requests_per_frame: 64
 *   repath_interval: 0.5
 *   source_scan_interval: 0.1  # seconds between collider/volume/link change scans
 *   repath_distance: 0.5
 *   flow:
 *     rebuild_distance: 0.5      # target movement that triggers a rebuild, metres
 *     max_distance: 0            # march bound (0 = none)
 *     wall_penalty: 1.0
 *     bound_to_followers: true   # exact fields stop once every follower is covered
 *     mode: auto                 # auto | exact | hierarchical (open worlds)
 *     exact_tile_budget: 64      # auto: exact when the routed area is at most this many tiles
 *     near_radius: 12            # hierarchical: always-exact zone around the goal, metres
 *     lookahead_tiles: 2         # hierarchical: exact tiles ahead of each follower
 *     rebuild_interval: 0.25     # min seconds between follower-driven rebuilds
 *   parallel_threshold: 64
 *   debug_draw: [Mesh, Links, Paths, Flow]
 *   debug_flow_stride: 2
 * @endcode
 */
inline NavSettings parse_nav_settings(const fkyaml::node& node, const std::vector<std::string>& physics_layer_names = {}) {
    NavSettings s;
    if (!node.is_mapping()) return s;
    if (node.contains("enabled")) s.enabled = node.at("enabled").get_value<bool>();
    if (node.contains("build_without_agents")) s.build_without_agents = node.at("build_without_agents").get_value<bool>();
    NavBuildSettings& b = s.build;
    b.cell_size = detail::read_float_(node, "cell_size", b.cell_size);
    b.cell_height = detail::read_float_(node, "cell_height", b.cell_height);
    if (node.contains("tile_size")) b.tile_size = static_cast<int>(node.at("tile_size").get_value<int64_t>());
    b.max_slope_degrees = detail::read_float_(node, "max_slope", b.max_slope_degrees);
    b.bounds_padding = detail::read_float_(node, "bounds_padding", b.bounds_padding);
    if (node.contains("bounds")) {
        const auto& bn = node.at("bounds");
        if (bn.contains("min") && bn.contains("max")) {
            b.bounds.min = detail::read_xyz_(bn.at("min"), glm::vec3(0.0f));
            b.bounds.max = detail::read_xyz_(bn.at("max"), glm::vec3(0.0f));
        }
    }
    if (node.contains("layers")) {
        b.layer_mask = 0;
        for (const auto& l : node.at("layers")) {
            if (l.is_integer()) {
                int64_t i = l.get_value<int64_t>();
                if (i >= 0 && i < 32) b.layer_mask |= 1u << i;
                continue;
            }
            std::string name = l.get_value<std::string>();
            for (std::size_t i = 0; i < physics_layer_names.size() && i < 32; ++i) {
                if (physics_layer_names[i] == name) b.layer_mask |= 1u << i;
            }
        }
    }
    if (node.contains("agents")) {
        b.agents.clear();
        for (const auto& a : node.at("agents")) {
            AgentProfile p;
            if (a.contains("name")) p.name = a.at("name").get_value<std::string>();
            p.radius = detail::read_float_(a, "radius", p.radius);
            p.height = detail::read_float_(a, "height", p.height);
            p.max_climb = detail::read_float_(a, "max_climb", p.max_climb);
            b.agents.push_back(p);
        }
        if (b.agents.empty()) b.agents.push_back(AgentProfile{});
    }
    if (node.contains("areas")) {
        uint32_t next_id = 2;
        for (const auto& a : node.at("areas")) {
            uint32_t id = next_id;
            if (a.contains("id")) id = static_cast<uint32_t>(a.at("id").get_value<int64_t>());
            if (id == k_area_null || id >= k_max_areas) continue;
            if (a.contains("name")) b.areas.names[id] = a.at("name").get_value<std::string>();
            b.areas.costs[id] = detail::read_float_(a, "cost", b.areas.costs[id]);
            next_id = std::max(next_id, id + 1);
        }
    }
    b.rebuild_delay = detail::read_float_(node, "rebuild_delay", b.rebuild_delay);
    if (node.contains("max_concurrent_rebuilds")) {
        b.max_concurrent_rebuilds = static_cast<int>(node.at("max_concurrent_rebuilds").get_value<int64_t>());
    }
    if (node.contains("max_path_requests_per_frame")) {
        s.max_path_requests_per_frame = static_cast<int>(node.at("max_path_requests_per_frame").get_value<int64_t>());
    }
    s.repath_interval = detail::read_float_(node, "repath_interval", s.repath_interval);
    s.source_scan_interval = detail::read_float_(node, "source_scan_interval", s.source_scan_interval);
    s.repath_distance = detail::read_float_(node, "repath_distance", s.repath_distance);
    if (node.contains("flow")) {
        const auto& f = node.at("flow");
        s.flow_rebuild_distance = detail::read_float_(f, "rebuild_distance", s.flow_rebuild_distance);
        s.flow_max_distance = detail::read_float_(f, "max_distance", s.flow_max_distance);
        s.flow_wall_penalty = detail::read_float_(f, "wall_penalty", s.flow_wall_penalty);
        if (f.contains("bound_to_followers")) s.flow_bound_to_followers = f.at("bound_to_followers").get_value<bool>();
        if (f.contains("mode")) {
            std::string mode = f.at("mode").get_value<std::string>();
            if (mode == "exact" || mode == "Exact") s.flow_mode = FlowFieldMode::Exact;
            else if (mode == "hierarchical" || mode == "Hierarchical") s.flow_mode = FlowFieldMode::Hierarchical;
            else s.flow_mode = FlowFieldMode::Auto;
        }
        if (f.contains("exact_tile_budget")) s.flow_exact_tile_budget = static_cast<uint32_t>(f.at("exact_tile_budget").get_value<int64_t>());
        s.flow_near_radius = detail::read_float_(f, "near_radius", s.flow_near_radius);
        if (f.contains("lookahead_tiles")) s.flow_lookahead_tiles = static_cast<int>(f.at("lookahead_tiles").get_value<int64_t>());
        s.flow_rebuild_interval = detail::read_float_(f, "rebuild_interval", s.flow_rebuild_interval);
    }
    if (node.contains("parallel_threshold")) {
        s.parallel_threshold = static_cast<std::size_t>(node.at("parallel_threshold").get_value<int64_t>());
    }
    if (node.contains("debug_draw")) {
        const auto& dd = node.at("debug_draw");
        NavDebugFlags flags = NavDebugFlags::None;
        if (dd.is_sequence()) {
            for (const auto& n : dd) flags = flags | parse_nav_debug_flag(n.get_value<std::string>());
        } else if (dd.is_string()) {
            flags = parse_nav_debug_flag(dd.get_value<std::string>());
        }
        s.debug_draw = flags;
    }
    if (node.contains("debug_flow_stride")) s.debug_flow_stride = static_cast<int>(node.at("debug_flow_stride").get_value<int64_t>());
    return s;
}

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_SETTINGS_H
