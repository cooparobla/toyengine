/**
 * @file nav_system_test.cpp
 * @brief NavSystem in a scene: YAML-loaded NavAgent/NavModifier components pathing and flowing to a
 *        moving target, and a dynamic carving crate that carves only once it sleeps.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/system/nav_system.h>
#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/debug/debug_draw.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/asset/asset_manager.h>
#include <memory>
#include <sstream>

COOPA_TEST_SUITE("nav_system");

using namespace coopa::physx;

COOPA_TEST(scene_agents_path_and_flow_to_a_moving_target) {
    using namespace coopa::scene;
    coopa::asset::AssetManager assets;
    register_physics_components(assets);
    // SceneLoader's parser table is process-global: unregister even when a check below ends the
    // test early, so no later test in this process inherits it.
    struct ParserReset {
        ~ParserReset() { coopa::scene::SceneLoader::clear_component_parsers(); }
    } reset_parsers;
    std::istringstream iss(
        "scene:\n"
        "  scene_name: NavTest\n"
        "  root_objects:\n"
        "    - name: ground\n"
        "      components:\n"
        "        - { type: Transform, position: { x: 0, y: 0, z: -0.5 } }\n"
        "        - { type: BoxCollider, size: { x: 20, y: 20, z: 1 } }\n"
        "    - name: crate\n"
        "      components:\n"
        "        - { type: Transform, position: { x: 0, y: 0, z: 1 } }\n"
        "        - { type: BoxCollider, size: { x: 4, y: 4, z: 2 } }\n"
        "        - { type: NavModifier, walkable: false }\n"
        "    - name: target\n"
        "      components:\n"
        "        - { type: Transform, position: { x: 7, y: 7, z: 0 } }\n"
        "    - name: walker\n"
        "      components:\n"
        "        - { type: Transform, position: { x: -6, y: -6, z: 0 } }\n"
        "        - { type: NavAgent, speed: 4, destination: { x: 6, y: 6, z: 0 } }\n"
        "    - name: mob0\n"
        "      components:\n"
        "        - { type: Transform, position: { x: -7, y: 5, z: 0 } }\n"
        "        - { type: NavAgent, speed: 4, flow_target: target }\n"
        "    - name: mob1\n"
        "      components:\n"
        "        - { type: Transform, position: { x: 5, y: -7, z: 0 } }\n"
        "        - { type: NavAgent, speed: 4, flow_target: target }\n");
    fkyaml::node root = fkyaml::node::deserialize(iss);
    Scene scene = SceneLoader::load_from_node(root, "");
    scene.start();
    system::install_physics_system(scene);
    auto* navsys = system::install_nav_system(scene);

    int arrivals = 0;
    auto* walker = scene.find_object("walker")->get_component<components::NavAgentComponent>();
    walker->on_arrived.connect([&](components::NavAgentComponent&) { ++arrivals; });
    for (int i = 0; i < 60 * 8; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
    ASSERT_TRUE(navsys->agent_count() == 3);
    ASSERT_TRUE(navsys->mesh() != nullptr);
    // The crate's top is not walkable even though it is flat and agent-sized.
    ASSERT_TRUE(navsys->mesh()->find_nearest({0.0f, 0.0f, 2.0f}, glm::vec3(0.5f)) == nav::k_invalid_span);
    ASSERT_TRUE(walker->arrived() && arrivals == 1);
    glm::vec3 wp = glm::vec3(scene.find_object("walker")->get_transform()->get_world_matrix()[3]);
    ASSERT_VEC_NEAR(wp, glm::vec3(6.0f, 6.0f, 0.0f), 0.2f);
    ASSERT_TRUE(navsys->flow_field("target") != nullptr);
    for (const char* name : {"mob0", "mob1"}) {
        glm::vec3 p = glm::vec3(scene.find_object(name)->get_transform()->get_world_matrix()[3]);
        ASSERT_TRUE(glm::distance(glm::vec2(p), glm::vec2(7.0f, 7.0f)) < 1.2f);
    }

    // Move the target: the shared flow field rebuilds and the mobs follow.
    scene.find_object("target")->get_transform()->transform().set_position(glm::vec3(-7.0f, -7.0f, 0.0f));
    for (int i = 0; i < 60 * 8; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
    for (const char* name : {"mob0", "mob1"}) {
        glm::vec3 p = glm::vec3(scene.find_object(name)->get_transform()->get_world_matrix()[3]);
        ASSERT_TRUE(glm::distance(glm::vec2(p), glm::vec2(-7.0f, -7.0f)) < 1.2f);
    }
    debug::DebugDraw draw;
    navsys->debug_draw(draw, nav::NavDebugFlags::All);
    ASSERT_TRUE(!draw.lines.empty());
}

COOPA_TEST(dynamic_crate_carves_only_once_asleep) {
    using namespace coopa::scene;
    Scene scene("NavCarve");
    auto ground = std::make_unique<SceneObject>("ground");
    ground->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, -0.5f));
    ground->add_component<components::BoxCollider>()->set_size(glm::vec3(16.0f, 16.0f, 1.0f));
    scene.add_root_object(std::move(ground));
    auto crate = std::make_unique<SceneObject>("crate");
    crate->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 1.5f));
    crate->add_component<components::BoxCollider>()->set_size(glm::vec3(3.0f, 3.0f, 1.0f));
    crate->add_component<components::RigidbodyComponent>()->mass = 5.0f;
    crate->add_component<components::NavModifierComponent>()->carve = true;
    scene.add_root_object(std::move(crate));
    scene.start();
    system::install_physics_system(scene);
    nav::NavSettings settings;
    settings.build.rebuild_delay = 0.1f;
    auto* navsys = system::install_nav_system(scene, settings);

    // Carved = no ground span left under the crate (its own top, 1 m up, stays walkable).
    auto blocked = [&]() {
        auto m = navsys->mesh();
        return m && m->find_nearest({0.0f, 0.0f, 0.0f}, glm::vec3(0.2f, 0.2f, 0.3f)) == nav::k_invalid_span;
    };
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    ASSERT_TRUE(!blocked()); // falling: not carved yet
    for (int i = 0; i < 60 * 6; ++i) {
        scene.update(1.0f / 60.0f);
        scene.late_update(1.0f / 60.0f);
    }
    navsys->flush();
    ASSERT_TRUE(blocked()); // came to rest and fell asleep: carved
}
