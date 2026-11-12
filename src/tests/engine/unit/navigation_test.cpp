/**
 * @file navigation_test.cpp
 * @brief Navigation as the engine installs it, on the shipped nav_demo scene: A* around costly
 *        mud and up to the deck, a chaser re-planning after a moving beacon, a one-way drop link,
 *        and 144 mobs on one flow field. The 246-mob open-world run is extended/navigation_open_world;
 *        the planners themselves are physxcoopa's own suite.
 */

#include <coopa/testing/test.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <glm/glm.hpp>
#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/systems/transform_system.h>
#include <physxcoopa/components/nav_agent.h>
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/system/nav_system.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/register.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("navigation");

/**
 * @brief The nav_demo scene, headless: loads the real scene file (with its own
 *        `scene.settings.navigation`), runs kinematic movers, physics and navigation for 25 s, and
 *        checks every station -- A* up stairs/ramp to the deck around the costly mud, a chaser
 *        re-planning after a moving beacon, a one-way drop link, and 144 mobs on one flow field
 *        converging on a circling target.
 */
COOPA_TEST(nav_test_agents_reach_their_goals) {
    // Parser lambdas capture the AssetManager by reference, so it must outlive the process's
    // registrations (later tests in this group may load scenes too).
    static coopa::asset::AssetManager assets;
    coopa::physx::register_physics_components(assets);
    toy::scene::register_scene_components();

    const std::string path = std::string(ROOT_DIR) + "/assets/scenes/navigation/nav_demo/scene.yaml";
    std::ifstream in(path);
    fkyaml::node root = fkyaml::node::deserialize(in);
    coopa::physx::nav::NavSettings nav_settings =
        coopa::physx::nav::parse_nav_settings(root["scene"]["settings"]["navigation"]);
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load(path);
    scene.start();
    toy::scene::install_kinematic_control_system(scene);
    coopa::physx::system::install_physics_system(scene);
    auto* nav = coopa::physx::system::install_nav_system(scene, nav_settings);
    coopa::scene::install_transform_system(scene);

    auto pos = [&](const char* name) {
        coopa::scene::SceneObject* o = scene.find_object(name);
        return o ? glm::vec3(o->get_transform()->get_world_matrix()[3]) : glm::vec3(1e9f);
    };
    using coopa::physx::components::NavAgentComponent;
    auto agent = [&](const char* name) { return scene.find_object(name)->get_component<NavAgentComponent>(); };

    const float dt = 1.0f / 60.0f;
    bool c_used_link = false;
    bool a_avoided_mud = true;
    for (int i = 0; i < 60 * 25; ++i) {
        scene.update(dt);
        scene.late_update(dt);
        const auto& pc = agent("courier_c")->path();
        for (uint8_t f : pc.flags) c_used_link |= (f & coopa::physx::nav::k_path_point_link_start) != 0;
        glm::vec3 a = pos("courier_a");
        // The mud is x -6..-1, y 6..16; half a metre in from its edges, so a body brushing the
        // edge while its path skirts it doesn't count.
        if (a.x > -5.5f && a.x < -1.5f && a.y > 6.5f && a.y < 15.5f) a_avoided_mud = false;
    }

    expect(nav->agent_count() == 147, "nav_demo: 3 couriers + 144 mobs bound");
    expect(nav->mesh() != nullptr, "nav_demo: the navmesh built from the scene's colliders");

    // courier_a: ground -> deck (z = 3 + base_offset 0.78), never through the mud.
    expect(agent("courier_a")->arrived(), "nav_demo: courier_a arrived");
    expect(glm::distance(pos("courier_a"), glm::vec3(7.5f, 14.5f, 3.78f)) < 0.4f, "nav_demo: courier_a is on the deck at its goal");
    expect(a_avoided_mud, "nav_demo: courier_a detoured around the cost-8 mud");
    // courier_b: chasing the circling beacon -- it must be up on the deck and close behind it.
    expect(pos("courier_b").z > 3.5f, "nav_demo: courier_b climbed to the deck");
    expect(glm::distance(glm::vec2(pos("courier_b")), glm::vec2(pos("beacon"))) < 2.5f, "nav_demo: courier_b keeps up with the beacon");
    // courier_c: deck -> ground east, via the one-way drop.
    expect(c_used_link, "nav_demo: courier_c planned through the drop link");
    expect(agent("courier_c")->arrived() && glm::distance(pos("courier_c"), glm::vec3(19.0f, 13.0f, 0.78f)) < 0.4f,
           "nav_demo: courier_c arrived on the ground east of the deck");

    // Mobs: crowd around the moving target.
    glm::vec2 t(pos("target"));
    int near = 0;
    for (int i = 0; i < 144; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "mob_%03d", i);
        glm::vec3 p = pos(name);
        if (glm::distance(glm::vec2(p), t) < 9.0f) ++near;
        expect(p.z > 0.5f && p.z < 0.8f, "nav_demo: every mob stays on the ground surface");
    }
    expect(near >= 120, "nav_demo: the mob crowd converged on the circling target");
    expect(nav->flow_field("target") != nullptr, "nav_demo: one shared flow field serves the mobs");
}
