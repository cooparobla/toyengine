/**
 * @file physics_config_test.cpp
 * @brief Authored physics configuration: PhysicsMaterial combine-mode priority, material assets
 *        loaded through AssetManager overriding a collider's inline material, and the physics
 *        settings YAML parsed and applied to a world.
 */
#include <coopa/testing/test.h>

#include <physxcoopa/world.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/loaders/physics_material_loader.h>
#include <physxcoopa/util/physics_settings.h>
#include <coopa/asset/asset_manager.h>
#include <fstream>
#include <sstream>

COOPA_TEST_SUITE("physics_config");

using namespace coopa::physx;

COOPA_TEST(material_combine_mode_priority_picks_the_stronger_mode) {
    dynamics::PhysicsMaterial a;
    a.dynamic_friction = 0.2f;
    a.restitution = 0.1f;
    dynamics::PhysicsMaterial b;
    b.dynamic_friction = 0.8f;
    b.restitution = 0.9f;

    // Priority order is Multiply > Maximum > Minimum > Average -- the higher-priority mode of
    // the two wins, regardless of which side (a or b) specified it.
    ASSERT_NEAR(dynamics::combine(a.dynamic_friction, b.dynamic_friction,
                                   dynamics::CombineMode::Average, dynamics::CombineMode::Average), 0.5f, 1e-5f);
    ASSERT_NEAR(dynamics::combine(a.dynamic_friction, b.dynamic_friction,
                                   dynamics::CombineMode::Minimum, dynamics::CombineMode::Average), 0.2f, 1e-5f);
    ASSERT_NEAR(dynamics::combine(a.restitution, b.restitution,
                                   dynamics::CombineMode::Maximum, dynamics::CombineMode::Average), 0.9f, 1e-5f);
    ASSERT_NEAR(dynamics::combine(a.restitution, b.restitution,
                                   dynamics::CombineMode::Multiply, dynamics::CombineMode::Maximum), 0.1f * 0.9f, 1e-5f);
}

COOPA_TEST(collider_material_asset_overrides_inline_material) {
    components::BoxCollider collider;
    ASSERT_TRUE(collider.material() == nullptr); // unset -> caller falls back to default_material()

    auto inline_mat = std::make_shared<dynamics::PhysicsMaterial>();
    inline_mat->restitution = 0.42f;
    collider.set_material(inline_mat);
    ASSERT_TRUE(collider.material() == inline_mat.get());

    // An asset handle, once loaded, takes priority over the inline material -- set up a real
    // AssetManager + PhysicsMaterialLoader + on-disk material so this exercises the actual
    // scene-authoring path (`material: <name>`), not just the accessor logic.
    namespace fs = std::filesystem;
    const fs::path dir = coopa::test::scratch_dir();
    fs::path file = dir / "ice.yaml";
    {
        std::ofstream out(file);
        out << "dynamic_friction: 0.02\n"
               "static_friction: 0.03\n"
               "restitution: 0.0\n"
               "friction_combine: Minimum\n"
               "restitution_combine: Average\n";
    }

    coopa::asset::AssetManager assets;
    assets.register_loader<dynamics::PhysicsMaterial>(std::make_unique<loaders::PhysicsMaterialLoader>());
    assets.add_search_root(dir.string());

    auto handle = assets.load<dynamics::PhysicsMaterial>("ice.yaml");
    ASSERT_TRUE(handle.is_loaded());
    ASSERT_NEAR(handle->dynamic_friction, 0.02f, 1e-6f);
    ASSERT_NEAR(handle->static_friction, 0.03f, 1e-6f);
    ASSERT_TRUE(handle->friction_combine == dynamics::CombineMode::Minimum);
    ASSERT_TRUE(handle->restitution_combine == dynamics::CombineMode::Average);

    collider.set_material_asset(handle);
    ASSERT_TRUE(collider.material() == handle.get());        // asset now wins over the inline material
    ASSERT_NEAR(collider.material()->dynamic_friction, 0.02f, 1e-6f);

}

COOPA_TEST(physics_settings_parse_and_apply_to_world) {
    std::istringstream iss(
        "gravity: { x: 0.0, y: 0.0, z: -3.0 }\n"
        "fixed_timestep: 0.02\n"
        "solver:\n"
        "  velocity_iterations: 4\n"
        "layers: [\"Default\", \"Ground\", \"Triggers\"]\n"
        "ignore_layer_collisions:\n"
        "  - [Triggers, Triggers]\n"
        "parallel_threshold: 8\n");
    fkyaml::node node = fkyaml::node::deserialize(iss);
    util::PhysicsSettings settings = util::parse_physics_settings(node);

    ASSERT_NEAR(settings.gravity.z, -3.0f, 1e-6f);
    ASSERT_NEAR(settings.solver.fixed_dt, 0.02f, 1e-6f);
    ASSERT_TRUE(settings.solver.velocity_iterations == 4);
    ASSERT_TRUE(settings.layer_names.size() == 3);
    ASSERT_TRUE(settings.parallel_threshold == 8);
    ASSERT_TRUE(settings.ignore_pairs.size() == 1);
    ASSERT_TRUE(settings.ignore_pairs[0].first == 2 && settings.ignore_pairs[0].second == 2); // "Triggers" == index 2

    PhysicsWorld world;
    apply_physics_settings(world, settings);
    ASSERT_NEAR(world.gravity().z, -3.0f, 1e-6f);
    ASSERT_TRUE(world.config().velocity_iterations == 4);
    ASSERT_TRUE(!world.layers().should_collide(2, 2));  // Triggers vs Triggers turned off
    ASSERT_TRUE(world.layers().should_collide(1, 2));   // untouched pair still collides
}
