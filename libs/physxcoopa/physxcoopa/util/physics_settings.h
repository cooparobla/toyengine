/**
 * @file physics_settings.h
 * @brief Scene/project-wide physics tunables -- Unity's "Project Settings > Physics" panel,
 *        loaded from a scene YAML's top-level `physics:` block and applied once at
 *        install_physics_system() time. Distinct from util::PhysicsConfig (still the raw
 *        per-substep solver tunables); PhysicsSettings wraps a PhysicsConfig plus the things
 *        that live above the solver: gravity, the fixed timestep, named layers, the layer
 *        collision matrix, default debug-draw flags and the job-parallelism threshold.
 */

#ifndef PHYSXCOOPA_UTIL_PHYSICS_SETTINGS_H
#define PHYSXCOOPA_UTIL_PHYSICS_SETTINGS_H

#include <physxcoopa/util/config.h>
#include <physxcoopa/util/math.h>
#include <physxcoopa/debug/debug_draw.h>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coopa {
namespace physx {

class PhysicsWorld; // world.h

namespace util {

/**
 * @struct PhysicsSettings
 * @brief Everything a scene's `physics:` YAML block can configure, applied once to a
 *        PhysicsWorld by apply_physics_settings() (called from install_physics_system()).
 */
struct PhysicsSettings {
    /** @brief Solver/broadphase tunables (velocity_iterations, fixed_dt, ...). */
    PhysicsConfig solver{};

    glm::vec3 gravity{0.0f, 0.0f, k_gravity_z};

    /** @brief Layer names, index == layer id (0..31). Lets scene YAML write
     *         `layer: "Ground"` instead of a bare integer -- see
     *         physx_yaml.h's layer-name registry, seeded from this list. */
    std::vector<std::string> layer_names;

    /** @brief Layer-id pairs whose collision is turned OFF (every pair collides by default). */
    std::vector<std::pair<uint32_t, uint32_t>> ignore_pairs;

    /** @brief Debug geometry PhysicsSystem should gather every frame by default (see
     *         PhysicsWorld::debug_draw()); the render layer may still override this at runtime. */
    debug::DebugDrawFlags debug_draw = debug::DebugDrawFlags::None;

    /** @brief Minimum bound-collider count before PhysicsSystem's job-parallel passes
     *         dispatch instead of running serially -- mirrors
     *         toyengine's JobsConfig::parallel_threshold idiom. */
    std::size_t parallel_threshold = 64;
};

namespace detail {

inline debug::DebugDrawFlags parse_debug_draw_flag_(const std::string& name) {
    if (name == "Colliders") return debug::DebugDrawFlags::Colliders;
    if (name == "BVH") return debug::DebugDrawFlags::BVH;
    if (name == "Contacts") return debug::DebugDrawFlags::Contacts;
    if (name == "Joints") return debug::DebugDrawFlags::Joints;
    if (name == "All") return debug::DebugDrawFlags::All;
    return debug::DebugDrawFlags::None;
}

} // namespace detail

/**
 * @brief Parses a `physics:` YAML node into a PhysicsSettings. Every key is optional; a missing
 *        or malformed field is left at its in-struct default, matching the rest of this
 *        codebase's config-loading policy (see toy::core::AppConfig::load()).
 *
 * @code
 * physics:
 *   gravity: { x: 0.0, y: 0.0, z: -9.81 }
 *   fixed_timestep: 0.016666
 *   solver:
 *     velocity_iterations: 16
 *     position_iterations: 4
 *   cloth:
 *     substeps: 4
 *     iterations: 1
 *   layers: ["Default", "Ground", "Triggers"]
 *   ignore_layer_collisions:
 *     - [Triggers, Triggers]
 *   debug_draw: [Colliders, Contacts]
 *   parallel_threshold: 64
 * @endcode
 */
inline PhysicsSettings parse_physics_settings(const fkyaml::node& node) {
    PhysicsSettings settings;

    if (node.contains("gravity")) {
        const auto& g = node.at("gravity");
        if (g.contains("x")) settings.gravity.x = g.at("x").get_value<float>();
        if (g.contains("y")) settings.gravity.y = g.at("y").get_value<float>();
        if (g.contains("z")) settings.gravity.z = g.at("z").get_value<float>();
    }

    if (node.contains("fixed_timestep")) {
        settings.solver.fixed_dt = node.at("fixed_timestep").get_value<float>();
    }

    if (node.contains("solver")) {
        const auto& s = node.at("solver");
        if (s.contains("velocity_iterations")) settings.solver.velocity_iterations = s.at("velocity_iterations").get_value<uint32_t>();
        if (s.contains("relax_iterations")) settings.solver.relax_iterations = s.at("relax_iterations").get_value<uint32_t>();
        if (s.contains("position_iterations")) settings.solver.position_iterations = s.at("position_iterations").get_value<uint32_t>();
        if (s.contains("position_correction")) settings.solver.position_correction = s.at("position_correction").get_value<float>();
        if (s.contains("linear_slop")) settings.solver.linear_slop = s.at("linear_slop").get_value<float>();
        if (s.contains("restitution_threshold")) settings.solver.restitution_threshold = s.at("restitution_threshold").get_value<float>();
        if (s.contains("sleep_linear")) settings.solver.sleep_linear = s.at("sleep_linear").get_value<float>();
        if (s.contains("sleep_angular")) settings.solver.sleep_angular = s.at("sleep_angular").get_value<float>();
        if (s.contains("sleep_time")) settings.solver.sleep_time = s.at("sleep_time").get_value<float>();
        if (s.contains("max_substeps")) settings.solver.max_substeps = s.at("max_substeps").get_value<uint32_t>();
        if (s.contains("max_linear_velocity")) settings.solver.max_linear_velocity = s.at("max_linear_velocity").get_value<float>();
        if (s.contains("max_kinematic_speed")) settings.solver.max_kinematic_speed = s.at("max_kinematic_speed").get_value<float>();
        if (s.contains("max_kinematic_spin")) settings.solver.max_kinematic_spin = s.at("max_kinematic_spin").get_value<float>();
    }

    // Cloth lives under its own key rather than inside `solver:` because its two knobs govern a
    // completely separate solver (XPBD, cloth/cloth_solver.h) with its own convergence tradeoff --
    // grouping them with the impulse solver's iteration counts would invite copying one set of
    // numbers onto the other, where they mean something different. See PhysicsConfig::cloth_substeps.
    if (node.contains("cloth")) {
        const auto& cl = node.at("cloth");
        if (cl.contains("substeps")) settings.solver.cloth_substeps = cl.at("substeps").get_value<uint32_t>();
        if (cl.contains("iterations")) settings.solver.cloth_iterations = cl.at("iterations").get_value<uint32_t>();
    }

    if (node.contains("layers")) {
        for (const auto& n : node.at("layers")) settings.layer_names.push_back(n.get_value<std::string>());
    }

    if (node.contains("ignore_layer_collisions")) {
        for (const auto& pair : node.at("ignore_layer_collisions")) {
            if (pair.size() < 2) continue;
            auto resolve = [&](const fkyaml::node& n) -> uint32_t {
                if (n.is_string()) {
                    std::string name = n.get_value<std::string>();
                    for (size_t i = 0; i < settings.layer_names.size(); ++i) {
                        if (settings.layer_names[i] == name) return static_cast<uint32_t>(i);
                    }
                    return 0;
                }
                return n.get_value<uint32_t>();
            };
            settings.ignore_pairs.emplace_back(resolve(pair.at(0)), resolve(pair.at(1)));
        }
    }

    if (node.contains("debug_draw")) {
        debug::DebugDrawFlags flags = debug::DebugDrawFlags::None;
        const auto& dd = node.at("debug_draw");
        if (dd.is_sequence()) {
            for (const auto& n : dd) flags = flags | detail::parse_debug_draw_flag_(n.get_value<std::string>());
        } else if (dd.is_string()) {
            flags = detail::parse_debug_draw_flag_(dd.get_value<std::string>());
        }
        settings.debug_draw = flags;
    }

    if (node.contains("parallel_threshold")) {
        settings.parallel_threshold = node.at("parallel_threshold").get_value<std::size_t>();
    }

    return settings;
}

} // namespace util
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_UTIL_PHYSICS_SETTINGS_H
