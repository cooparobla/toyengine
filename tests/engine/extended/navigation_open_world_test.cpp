/**
 * @file navigation_open_world_test.cpp
 * @brief nav_stress_demo, headless: 246 mobs over a 400 x 400 m world on two hierarchical flow
 *        fields for 40 simulated seconds. The player field must stay hierarchical and integrate only
 *        a small fraction of the world's tiles while every pack closes on its target. Extended tier:
 *        ~8 s of CPU; the hierarchical field itself is unit-tested in physxcoopa.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/systems/transform_system.h>
#include <physxcoopa/components/nav_agent.h>
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/system/nav_system.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/register.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("navigation_open_world");

/**
 * @brief nav_stress_demo, headless: 246 mobs over a 400 x 400 m world on two hierarchical flow
 *        fields. The player field must stay hierarchical and integrate only a small fraction of
 *        the world's tiles while every pack closes on its target -- through the wall's gaps, down
 *        the mesa's ramps, across the whole map for the scouts.
 */
COOPA_TEST(open_world_packs_converge_on_hierarchical_fields) {
    static coopa::asset::AssetManager assets;
    coopa::physx::register_physics_components(assets);
    toy::scene::register_scene_components();

    const std::string path = std::string(ROOT_DIR) + "/assets/scenes/navigation/nav_stress_demo/scene.yaml";
    std::ifstream in(path);
    fkyaml::node root = fkyaml::node::deserialize(in);
    coopa::physx::nav::NavSettings ns = coopa::physx::nav::parse_nav_settings(root["scene"]["settings"]["navigation"]);
    coopa::job::JobEngine jobs;
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load(path);
    scene.set_job_engine(&jobs);
    scene.start();
    toy::scene::install_kinematic_control_system(scene);
    coopa::physx::system::install_physics_system(scene);
    auto* nav = coopa::physx::system::install_nav_system(scene, ns);
    coopa::scene::install_transform_system(scene);

    using coopa::physx::components::NavAgentComponent;
    struct Mob { coopa::scene::SceneObject* obj; std::string target; float start; glm::vec3 start_pos; };
    std::vector<Mob> mobs;
    auto pos = [](coopa::scene::SceneObject* o) { return glm::vec3(o->get_transform()->get_world_matrix()[3]); };
    for (auto* a : scene.get_components<NavAgentComponent>()) mobs.push_back({a->owner, a->flow_target, 0.0f, pos(a->owner)});
    expect(mobs.size() == 246, "nav_stress_demo: 246 mobs");

    const float dt = 1.0f / 60.0f;
    std::size_t max_active = 0, hier_samples = 0;
    for (int i = 0; i < 60 * 40; ++i) {
        scene.update(dt);
        scene.late_update(dt);
        if (i == 0) {
            for (auto& m : mobs) m.start = glm::distance(glm::vec2(pos(m.obj)), glm::vec2(pos(scene.find_object(m.target))));
        }
        if (i % 60 == 0) {
            if (auto f = nav->flow_field("player"); f && f->hierarchical()) {
                ++hier_samples;
                max_active = std::max(max_active, f->active_tile_count());
            }
        }
    }
    const uint32_t tiles = nav->mesh()->params.tile_count();
    expect(hier_samples >= 30, "nav_stress_demo: the player field is hierarchical");
    expect(max_active * 4 < tiles, "nav_stress_demo: it integrates well under a quarter of the world's tiles");
    expect(nav->flow_field("outpost") != nullptr, "nav_stress_demo: the scouts' field exists");

    // Every pack closes on its target.
    double player_start = 0, player_now = 0, scout_start = 0, scout_now = 0;
    int on_mesa_start = 0, on_mesa_now = 0, player_n = 0, scout_n = 0;
    for (auto& m : mobs) {
        float now = glm::distance(glm::vec2(pos(m.obj)), glm::vec2(pos(scene.find_object(m.target))));
        if (m.target == "player") { player_start += m.start; player_now += now; ++player_n; }
        else { scout_start += m.start; scout_now += now; ++scout_n; }
        if (m.start_pos.z > 3.0f) { ++on_mesa_start; on_mesa_now += pos(m.obj).z > 3.0f; }
    }
    if (coopa::test::verbose()) std::printf("  open world: player mean dist %.1f -> %.1f m, scouts %.1f -> %.1f m, mesa %d -> %d, max active tiles %zu/%u\n",
                player_start / player_n, player_now / player_n, scout_start / scout_n, scout_now / scout_n,
                on_mesa_start, on_mesa_now, max_active, tiles);
    expect(player_now < 0.4 * player_start, "nav_stress_demo: the player's packs closed most of the distance");
    // 40 s at 4 m/s is 160 m of walking; the scouts' route is ~460 m.
    expect((scout_start - scout_now) / scout_n > 110.0, "nav_stress_demo: the scouts crossed a good part of the map");
    expect(on_mesa_start > 0 && on_mesa_now * 2 < on_mesa_start, "nav_stress_demo: the mesa pack came down its ramps");
}
