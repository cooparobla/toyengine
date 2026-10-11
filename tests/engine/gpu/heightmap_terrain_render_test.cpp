/**
 * @file heightmap_terrain_render_test.cpp
 * @brief heightmap_terrain_demo end to end: the terrain's chunks batch into instanced draws and
 *        render a real surface (no unwritten / NaN blocks), their boxes contain the displaced
 *        grid, an edit to the grid reaches the image the next frames, the heightfield collider
 *        answers physics raycasts at the grid's height, deep snow and rain change the surface.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>

#include <glm/glm.hpp>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/world/heightmap_store.h>
#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/heightmap_terrain_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scene_variant.h"

COOPA_TEST_SUITE("heightmap_terrain_render");

using namespace toy::test;

namespace {

const char* k_demo = "assets/scenes/terrain/heightmap_terrain_demo/scene.yaml";

std::string demo() { return std::string(ROOT_DIR) + "/" + k_demo; }

toy::world::HeightmapTerrain* terrain_of(toy::core::Engine& engine) {
    return engine.scene().find_first_component<toy::world::HeightmapTerrain>();
}

} // namespace

/** @brief The demo draws a solid, lit terrain whose chunks batch and whose boxes contain it. */
COOPA_TEST(demo_renders_the_terrain) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::HeightmapStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    toy::world::HeightmapTerrain* t = terrain_of(engine);
    require(t != nullptr && t->data != nullptr && t->runtime != nullptr, "heightmap terrain: loaded and running");
    expect(t->runtime->chunks.size() == 256, "heightmap terrain: 16 x 16 chunks of 64 m");
    expect(t->runtime->slot == 0, "heightmap terrain: slot 0 of the terrain table");
    const auto& ubo = engine.pipeline().surface_world_ubo();
    expect(ubo.terrain_count.x == 1 && ubo.terrain[0].dims.x == 1025, "heightmap terrain: its numbers reach the surface UBO");

    // Every chunk's culling box holds its part of the grid.
    bool boxes_ok = true;
    for (int cy = 0; cy < 16 && boxes_ok; cy += 5) {
        for (int cx = 0; cx < 16 && boxes_ok; cx += 5) {
            const coopa::scene::SceneObject* c = t->runtime->chunks[static_cast<std::size_t>(cy) * 16 + cx];
            const glm::mat4 w = c->get_transform()->transform().get_world_matrix();
            const glm::vec2 mm = t->data->chunk_minmax(cx, cy);
            const float lo = w[3].z, hi = w[3].z + w[2].z;
            boxes_ok = lo <= mm.x + t->origin().z && hi >= mm.y + t->origin().z;
        }
    }
    expect(boxes_ok, "heightmap terrain: chunk boxes contain the displaced grid");

    const Frame f = engine.capture_image();
    expect(black_block_pixels(f) < 64, "heightmap terrain: no unwritten (black) blocks");
    const glm::vec3 ground = band_mean(f, 0.75f, 0.95f, 0.2f, 0.8f);
    expect(ground.g > ground.b && ground.g > 30.0f, "heightmap terrain: the ground under the player is lit grass (" +
           std::to_string(ground.r) + ", " + std::to_string(ground.g) + ", " + std::to_string(ground.b) + ")");
    dump_frame(f, "heightmap_terrain_demo");
}

/** @brief The collider is the grid: a physics ray finds the terrain at the grid's height. */
COOPA_TEST(heightfield_collider_matches_the_grid) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::HeightmapStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 30);
    toy::world::HeightmapTerrain* t = terrain_of(engine);
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(engine.scene().find_system("Physics"));
    require(t != nullptr && physics != nullptr, "heightmap terrain: terrain and physics present");
    for (const glm::vec2 xy : {glm::vec2(0.0f, 0.0f), glm::vec2(-300.0f, 200.0f), glm::vec2(350.0f, -100.0f)}) {
        coopa::physx::geometry::Ray ray;
        ray.origin = glm::vec3(xy, 400.0f);
        ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
        ray.max_distance = 1000.0f;
        coopa::physx::system::PhysicsSystem::RaycastHit hit;
        const bool found = physics->raycast(ray, hit);
        expect(found, "heightmap terrain: the ray hits the terrain");
        if (found) {
            expect_near(hit.point.z, t->view().height(glm::vec3(xy, 0.0f)), 0.05f,
                        "heightmap terrain: ...at the grid's height");
        }
    }
}

/** @brief Raising the grid in front of the camera changes the image within a few frames. */
COOPA_TEST(grid_edits_reach_the_image) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::HeightmapStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);
    toy::world::HeightmapTerrain* t = terrain_of(engine);
    require(t != nullptr, "heightmap terrain: loaded");
    const Frame before = engine.capture_image();
    // A 30 m mound around world (60, -20), in front of the player.
    toy::world::HeightmapData& d = *t->data;
    const glm::vec2 c = glm::vec2(60.0f, -20.0f) - glm::vec2(t->origin());
    toy::world::IRect r{static_cast<int>(c.x) - 40, static_cast<int>(c.y) - 40, static_cast<int>(c.x) + 40, static_cast<int>(c.y) + 40};
    for (int j = r.y0; j < r.y1; ++j) {
        for (int i = r.x0; i < r.x1; ++i) {
            const float dd = glm::length(glm::vec2(static_cast<float>(i), static_cast<float>(j)) - c) / 40.0f;
            if (dd < 1.0f) d.at(i, j) += 30.0f * (1.0f - dd * dd);
        }
    }
    d.mark_height_dirty(r);
    tick_frames(engine, 8);
    const Frame after = engine.capture_image();
    expect(count_diff(before, after, 12) > 2000, "heightmap terrain: the mound shows (" +
           std::to_string(count_diff(before, after, 12)) + " pixels changed)");
    toy::world::HeightmapStore::instance().clear();   // the edit must not leak into other tests
}

/** @brief Deep snow whitens the ground; a storm darkens it (wet). */
COOPA_TEST(snow_and_rain_change_the_surface) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    glm::vec3 dry, snowy, wet;
    {
        toy::world::HeightmapStore::instance().clear();
        toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
        tick_frames(engine, 90);
        dry = band_mean(engine.capture_image(), 0.75f, 0.95f, 0.2f, 0.8f);
        engine.render_config().snow_cover_override = 1.0f;
        tick_frames(engine, 30);
        snowy = band_mean(engine.capture_image(), 0.75f, 0.95f, 0.2f, 0.8f);
    }
    {
        toy::world::HeightmapStore::instance().clear();
        const std::string storm = scene_variant(k_demo, "heightmap_terrain_storm", [](fkyaml::node& doc) {
            doc["scene"]["settings"]["weather"]["condition"] = std::string("storm");
        });
        toy::core::Engine engine(make_test_config(storm, 320, 180, 320, 180));
        tick_frames(engine, 90);
        expect(engine.pipeline().surface_world_ubo().snow.z > 0.5f, "heightmap terrain: the storm's wetness reaches the UBO");
        wet = band_mean(engine.capture_image(), 0.75f, 0.95f, 0.2f, 0.8f);
    }
    const float l_dry = dry.r + dry.g + dry.b, l_snow = snowy.r + snowy.g + snowy.b, l_wet = wet.r + wet.g + wet.b;
    expect(l_snow > l_dry * 1.5f, "heightmap terrain: deep snow whitens the ground (" + std::to_string(l_dry) + " -> " +
           std::to_string(l_snow) + ")");
    expect(l_wet < l_dry, "heightmap terrain: rain darkens the ground (" + std::to_string(l_dry) + " -> " + std::to_string(l_wet) + ")");
}
