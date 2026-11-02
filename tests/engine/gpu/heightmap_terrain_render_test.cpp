/**
 * @file heightmap_terrain_render_test.cpp
 * @brief heightmap_terrain_demo end to end: the terrain's chunks batch into instanced draws and
 *        render a real surface (no unwritten / NaN blocks), their boxes contain the displaced
 *        grid, an edit to the grid reaches the image the next frames, the heightfield collider
 *        answers physics raycasts at the grid's height, deep snow and rain change the surface.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include <glm/glm.hpp>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/env/env_preset.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/water/water_body.h>
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

/** @brief Efficiency: a fixed view distance caps the chunks, scatter instances live in persistent
 *         buffers (not the per-frame stream), and `cast_shadows: false` drops the terrain from the
 *         shadow passes. */
COOPA_TEST(view_distance_static_instances_and_shadow_control) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::HeightmapStore::instance().clear();
    {
        toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
        tick_frames(engine, 120);
        toy::world::HeightmapTerrain* t = terrain_of(engine);
        require(t != nullptr && t->runtime != nullptr, "heightmap terrain: loaded");
        expect(t->runtime->live_chunks == 256, "heightmap terrain: auto view distance loads every chunk the camera can reach");
        const auto& stats = engine.pipeline().last_frame_stats();
        expect(stats.static_instances > 1000, "heightmap terrain: scatter draws from persistent buffers (" +
                                                  std::to_string(stats.static_instances) + ")");
        expect(stats.streamed_instances < 2000, "heightmap terrain: ...so the per-frame stream stays small (" +
                                                    std::to_string(stats.streamed_instances) + ")");
    }
    toy::world::HeightmapStore::instance().clear();
    {
        const std::string capped = scene_variant(k_demo, "heightmap_terrain_capped", [](fkyaml::node& doc) {
            fkyaml::node* t = scene_component(doc, "terrain", "Terrain");
            (*t)["view_distance"] = 200.0;
            (*t)["cast_shadows"] = false;
        });
        toy::core::Engine engine(make_test_config(capped, 320, 180, 320, 180));
        tick_frames(engine, 60);
        toy::world::HeightmapTerrain* t = terrain_of(engine);
        require(t != nullptr && t->runtime != nullptr, "heightmap terrain: loaded");
        const int cap = (2 * 4 + 3) * (2 * 4 + 3);   // ceil(200 / 64) = 4 chunks each way, plus slack
        expect(t->runtime->live_chunks > 0 && t->runtime->live_chunks <= cap,
               "heightmap terrain: a 200 m view distance loads only nearby chunks (" + std::to_string(t->runtime->live_chunks) + ")");
        const Frame f = engine.capture_image();
        const glm::vec3 ground = band_mean(f, 0.75f, 0.95f, 0.2f, 0.8f);
        expect(ground.g > 30.0f, "heightmap terrain: the ground near the camera still draws");
        bool any_caster = false;
        for (coopa::scene::SceneObject* c : t->runtime->chunks) {
            if (c == nullptr) continue;
            auto* mr = c->get_component<coopa::gfx::engine::components::MeshRenderer>();
            any_caster |= mr && mr->cast_shadows;
        }
        expect(!any_caster, "heightmap terrain: cast_shadows: false reaches every chunk");
    }
    toy::world::HeightmapStore::instance().clear();
}

namespace {

/** @brief The demo under one custom condition with `wind` m/s (heading 30 degrees). */
std::string wind_variant(const char* tag, float wind) {
    return scene_variant(k_demo, tag, [wind](fkyaml::node& doc) {
        fkyaml::node& w = doc["scene"]["settings"]["weather"];
        fkyaml::node c = fkyaml::node::mapping();
        c["name"] = std::string("test_wind");
        c["wind_strength"] = static_cast<double>(wind);
        c["wind_heading"] = 30.0;
        c["wind_gust"] = 0.0;
        c["fog_density"] = 0.0;
        fkyaml::node list = fkyaml::node::sequence();
        list.as_seq().push_back(c);
        w["conditions"] = list;
        w["condition"] = std::string("test_wind");
        w["schedule"] = std::string("fixed");
    });
}

/** @brief One capture of a wind variant after 120 frames, the sun held still and the
 *         frame-varying stylization off, so runs differ only by what the wind moved. */
Frame wind_capture(const std::string& scene) {
    toy::world::HeightmapStore::instance().clear();
    toy::core::Engine engine(make_test_config(scene, 320, 180, 320, 180));
    tick_frames(engine, 8);
    if (auto* w = engine.weather()) w->set_clock_paused(true);
    auto& rc = engine.render_config();
    rc.dither_enabled = false;
    rc.palette_enabled = false;
    rc.outline_enabled = false;
    tick_frames(engine, 112);
    Frame f = engine.capture_image();
    toy::world::HeightmapStore::instance().clear();
    return f;
}

/** @brief Changed pixels in the lower (ground / grass) part of two same-frame captures. */
long long ground_diff(const Frame& a, const Frame& b) {
    if (!same_extent(a, b)) return -1;
    long long n = 0;
    for (uint32_t y = a.height * 55 / 100; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            const std::size_t k = (static_cast<std::size_t>(y) * a.width + x) * a.channels;
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(int(a.pixels[k + c]) - int(b.pixels[k + c])));
            n += d > 10;
        }
    }
    return n;
}

} // namespace

/** @brief Grass and foliage bend in the weather's wind: at the same frame, a windy run's grass
 *         differs from a calm run's, while two calm runs are identical, and the wind reaches the
 *         surface UBO. */
COOPA_TEST(foliage_sways_in_the_weather_wind) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::string calm_scene = wind_variant("heightmap_terrain_calm", 0.0f);
    const std::string windy_scene = wind_variant("heightmap_terrain_windy", 12.0f);
    {
        toy::world::HeightmapStore::instance().clear();
        toy::core::Engine engine(make_test_config(windy_scene, 160, 90, 160, 90));
        tick_frames(engine, 30);
        expect(glm::length(glm::vec2(engine.pipeline().surface_world_ubo().wind)) > 8.0f,
               "foliage: the windy condition's wind reaches the surface UBO");
    }
    const Frame calm_a = wind_capture(calm_scene);
    const Frame calm_b = wind_capture(calm_scene);
    const Frame windy = wind_capture(windy_scene);
    // The demo's own clear weather is a light 1.5 m/s breeze: it must visibly stir the grass too.
    const Frame breeze = wind_capture(wind_variant("heightmap_terrain_breeze", 1.5f));
    const long long same = ground_diff(calm_a, calm_b);
    const long long swayed = ground_diff(calm_a, windy);
    const long long stirred = ground_diff(calm_a, breeze);
    expect(same < 50, "foliage: two calm runs agree (" + std::to_string(same) + " pixels)");
    expect(swayed > 500, "foliage: the wind moves the grass (" + std::to_string(swayed) + " pixels differ from calm)");
    expect(stirred > 300, "foliage: a light breeze stirs it visibly too (" + std::to_string(stirred) + " pixels)");
    if (swayed <= 500) {
        dump_frame(calm_a, "calm");
        dump_frame(windy, "windy");
    }
}

/**
 * @brief Deactivating a heightmap terrain must drop its chunks AND its collider, not leak them.
 *
 * `scene.get_components<HeightmapTerrain>()` filters on the component's own object being
 * active, so the moment a heightmap terrain is deactivated (an EnvPresetSwitcher preset group,
 * a scripted toggle) HeightmapTerrainSystem's per-frame gather stops seeing it -- and without
 * this, the chunk objects, their GPU mesh/texture state and the whole-grid `HeightfieldCollider`
 * stay on the tree and keep answering physics queries for a terrain nothing can see. Reactivating
 * must rebuild everything, since the whole point is "pay a reload, not a permanent leak".
 */
COOPA_TEST(deactivating_a_heightmap_terrain_drops_its_chunks_and_collider) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::HeightmapStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 160, 90, 160, 90));
    tick_frames(engine, 30);

    toy::world::HeightmapTerrain* t = terrain_of(engine);
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(engine.scene().find_system("Physics"));
    require(t != nullptr && t->runtime != nullptr && t->owner != nullptr && physics != nullptr,
            "heightmap terrain: terrain and physics present");
    require(!t->runtime->chunks.empty() && t->runtime->collision != nullptr,
            "heightmap terrain: chunks and a collider exist before deactivation");

    t->owner->set_active(false);
    tick_frames(engine, 5);   // destroy_chunks_() and the collider drop are synchronous

    expect(t->runtime->chunks.empty(), "heightmap terrain: deactivating drops every chunk");
    expect(t->runtime->live_chunks == 0, "heightmap terrain: ...and live_chunks resets");
    expect(t->runtime->collision == nullptr, "heightmap terrain: ...and the collider is released");

    coopa::physx::geometry::Ray ray;
    ray.origin = glm::vec3(0.0f, 0.0f, 400.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray.max_distance = 1000.0f;
    coopa::physx::system::PhysicsSystem::RaycastHit hit;
    expect(!physics->raycast(ray, hit), "heightmap terrain: the dropped collider no longer answers raycasts");

    t->owner->set_active(true);
    tick_frames(engine, 30);
    expect(!t->runtime->chunks.empty() && t->runtime->collision != nullptr,
           "heightmap terrain: reactivating rebuilds the chunks and the collider");
    expect(physics->raycast(ray, hit), "heightmap terrain: ...and raycasts hit it again");
}

/**
 * @brief A heightmap terrain carrying a `water:` block (unified_terrain_demo's smooth flavour,
 *        baked by the shared MapGenerator) must draw its lakes and rivers in the SAME world
 *        space as its own ground -- not stretched and lifted by the chunk's own culling-box
 *        transform. That box is scaled to the chunk's XY extent and its height range for
 *        frustum culling alone; build_smooth_chunk_water() builds in absolute metres already,
 *        so parenting the water under it (as refresh_chunk_water_() once did) stretched a 32 m
 *        sheet to chunk_tiles x chunk-widths and lifted its level by the height range, putting
 *        it far above where the camera could ever see it. Regression coverage for that: every
 *        existing heightmap test uses heightmap_terrain_demo, whose Terrain has no `water:` at
 *        all, so this path had never run in a scene before.
 */
COOPA_TEST(unified_demo_heightmap_water_lands_in_world_space) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::HeightmapStore::instance().clear();
    const std::string unified = std::string(ROOT_DIR) + "/assets/scenes/terrain/unified_terrain_demo/scene.yaml";
    toy::core::Engine engine(make_test_config(unified, 160, 90, 160, 90));

    // Through the real switcher, not a raw set_active(): an object authored `active: false` is
    // skipped by SceneObject::start() at load, and set_active() alone never starts it later --
    // only EnvPresetSwitcher's select() does, which is exactly what a real flavour switch is.
    // One tick first, so the switcher's own pending_initial_ (its `start: soft`) resolves before
    // this test's selection -- otherwise the first on_update() overwrites it right back to soft.
    auto* switcher = engine.scene().find_first_component<toy::env::EnvPresetSwitcher>();
    require(switcher != nullptr, "unified: the scene carries its EnvPresetSwitcher");
    tick_frames(engine, 1);
    require(switcher->select_by_name("smooth"), "unified: switched to the heightmap flavour");

    toy::world::TerrainComponent* t = nullptr;
    tick_until(engine, 600, [&] {
        t = nullptr;
        for (auto* c : engine.scene().get_components<toy::world::TerrainComponent>()) {
            if (c->mode == toy::world::TerrainMode::Heightmap) { t = c; break; }
        }
        return t != nullptr && t->runtime != nullptr && t->applied_map != nullptr && t->runtime->live_chunks > 0;
    });
    require(t != nullptr && t->runtime != nullptr, "unified smooth: the heightmap terrain is running");
    require(t->applied_map != nullptr, "unified smooth: the shared MapGenerator baked onto it");
    require(t->runtime->live_chunks > 0, "unified smooth: chunks streamed in");

    // Water lands a frame after its chunk: refresh_water_() runs after stream_chunks_() in tick_.
    tick_frames(engine, 10);

    const float chunk_size = t->data->geometry().chunk_size;
    const float height = t->data->geometry().height;
    const glm::vec3 origin = t->origin();

    int water_bodies = 0;
    bool all_in_range = true;
    for (coopa::scene::SceneObject* chunk : t->runtime->chunks) {
        if (chunk == nullptr) continue;
        for (const char* name : {"water", "river"}) {
            coopa::scene::SceneObject* w = chunk->find_child(name);
            if (w == nullptr) continue;
            auto* body = w->get_component<toy::water::WaterBody>();
            if (body == nullptr || body->geometry_positions.empty()) continue;
            ++water_bodies;
            const glm::mat4 world = w->get_transform()->transform().get_world_matrix();
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (const glm::vec3& p : body->geometry_positions) {
                const glm::vec3 wp = glm::vec3(world * glm::vec4(p, 1.0f));
                lo = glm::min(lo, wp);
                hi = glm::max(hi, wp);
            }
            // One chunk's sheet (plus its one-tile skirt into neighbours) spans a few tens of
            // metres -- never the whole grid, which is what the stretch bug produced.
            const bool size_ok = (hi.x - lo.x) < chunk_size * 2.0f && (hi.y - lo.y) < chunk_size * 2.0f;
            // The level is within the terrain's own relief, not lifted by the chunk's
            // height-range scale (which the bug multiplied the level by).
            const bool z_ok = lo.z >= origin.z - 5.0f && hi.z <= origin.z + height + 5.0f;
            if (!(size_ok && z_ok)) {
                all_in_range = false;
                std::cout << "unified smooth: " << chunk->name() << "/" << name << " out of range -- "
                          << "xy=(" << (hi.x - lo.x) << "," << (hi.y - lo.y) << ") z=[" << lo.z << "," << hi.z << "]\n";
            }
        }
    }
    expect(water_bodies > 0, "unified smooth: the heightmap flavour grew water bodies at all");
    expect(all_in_range, "unified smooth: every water sheet sits in world space, not the chunk's scaled box");
}
