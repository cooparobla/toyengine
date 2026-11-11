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
#include <cstdio>
#include <iostream>
#include <string>

#include <glm/glm.hpp>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/env/env_preset.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/water/water_body.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/world/terra_store.h>
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
    toy::world::TerraStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);

    toy::world::HeightmapTerrain* t = terrain_of(engine);
    require(t != nullptr && !t->field.empty() && t->runtime != nullptr, "heightmap terrain: loaded and running");
    expect(t->runtime->chunks.size() == 256, "heightmap terrain: 16 x 16 chunks of 64 m");
    expect(t->runtime->slot == 0, "heightmap terrain: slot 0 of the terrain table");
    const auto& ubo = engine.pipeline().surface_world_ubo();
    expect(ubo.terrain_count.x == 1 && ubo.terrain[0].dims.x == 1025, "heightmap terrain: its numbers reach the surface UBO");

    // Every chunk's culling box holds its part of the grid.
    bool boxes_ok = true;
    for (int cy = 0; cy < 16 && boxes_ok; cy += 5) {
        for (int cx = 0; cx < 16 && boxes_ok; cx += 5) {
            const coopa::scene::SceneObject* c = t->runtime->chunks[static_cast<std::size_t>(cy) * 16 + cx];
            if (c == nullptr) continue; // not yet streamed in at this frame count -- nothing to check
            const glm::mat4 w = c->get_transform()->transform().get_world_matrix();
            const glm::vec2 mm = t->field.chunk_height_range(cx, cy);
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
    toy::world::TerraStore::instance().clear();
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
            expect_near(hit.point.z, t->field_view().height(glm::vec3(xy, 0.0f)), 0.05f,
                        "heightmap terrain: ...at the grid's height");
        }
    }
}

/** @brief Raising the grid in front of the camera changes the image within a few frames. */
COOPA_TEST(grid_edits_reach_the_image) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::world::TerraStore::instance().clear();
    toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
    tick_frames(engine, 90);
    toy::world::HeightmapTerrain* t = terrain_of(engine);
    require(t != nullptr, "heightmap terrain: loaded");
    const Frame before = engine.capture_image();
    // A 30 m mound around world (60, -20), in front of the player.
    toy::world::TerraData& d = *t->field.data();
    const int sub = d.geometry().height_sub;
    const glm::vec2 c = glm::vec2(60.0f, -20.0f) - glm::vec2(t->origin());
    toy::world::IRect r{(static_cast<int>(c.x) - 40) * sub, (static_cast<int>(c.y) - 40) * sub,
                        (static_cast<int>(c.x) + 40) * sub, (static_cast<int>(c.y) + 40) * sub};
    for (int j = r.y0; j < r.y1; ++j) {
        for (int i = r.x0; i < r.x1; ++i) {
            const glm::vec2 p_m(static_cast<float>(i) / static_cast<float>(sub), static_cast<float>(j) / static_cast<float>(sub));
            const float dd = glm::length(p_m - c) / 40.0f;
            if (dd < 1.0f) d.height_at(i, j) += 30.0f * (1.0f - dd * dd);
        }
    }
    d.mark_height_dirty(r);
    tick_frames(engine, 8);
    const Frame after = engine.capture_image();
    expect(count_diff(before, after, 12) > 2000, "heightmap terrain: the mound shows (" +
           std::to_string(count_diff(before, after, 12)) + " pixels changed)");
    toy::world::TerraStore::instance().clear();   // the edit must not leak into other tests
}

/** @brief Deep snow whitens the ground; a storm darkens it (wet). */
COOPA_TEST(snow_and_rain_change_the_surface) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    glm::vec3 dry, snowy, wet;
    {
        toy::world::TerraStore::instance().clear();
        toy::core::Engine engine(make_test_config(demo(), 320, 180, 320, 180));
        tick_frames(engine, 90);
        dry = band_mean(engine.capture_image(), 0.75f, 0.95f, 0.2f, 0.8f);
        engine.render_config().snow_cover_override = 1.0f;
        tick_frames(engine, 30);
        snowy = band_mean(engine.capture_image(), 0.75f, 0.95f, 0.2f, 0.8f);
    }
    {
        toy::world::TerraStore::instance().clear();
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
    toy::world::TerraStore::instance().clear();
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
    toy::world::TerraStore::instance().clear();
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
    toy::world::TerraStore::instance().clear();
}

namespace {

/** @brief The demo under one custom condition with `wind` m/s (heading 30 degrees), plus one
 *         large `nature/grass_tuft` MeshRenderer -- not a ParticleSystem
 *         scatter instance -- planted directly in the demo camera's view. The scene's own
 *         scattered grass is real but too sparse and distant at this camera framing to measure
 *         reliably (confirmed separately: its own lit-image check only ever clears ~60 of its
 *         500-pixel threshold, a pre-existing fixture limitation unrelated to wind sway). A
 *         controlled, close, deliberately large plant isolates the one thing this test needs to
 *         prove -- a real previous-frame pose for a time-animated hook -- without depending on
 *         scatter density or distance. flexibility 1.0 and height 2 m at 10x scale give a
 *         canopy any camera motion or discretization noise can't hide. */
std::string velocity_test_scene(const char* tag, float wind) {
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

        // The demo camera's CameraController has no explicit `tracker`/`target`, which means
        // it follows the "player" object BY NAME, by convention -- removing that object (an
        // earlier version of this fixture did, to silence its animation as scene-motion noise)
        // leaves the controller's orbit target undefined and the camera drifts off its authored
        // pose over a multi-second capture. Keep the player: its identical, deterministic
        // animation is present in both the calm and windy copies of this scene and cancels out
        // in the comparison below exactly as it already does for foliage_sways_in_the_weather_wind.

        fkyaml::node obj = fkyaml::node::mapping();
        obj["name"] = std::string("velocityTestPlant");
        fkyaml::node xform = fkyaml::node::mapping();
        xform["type"] = std::string("Transform");
        fkyaml::node pos = fkyaml::node::mapping();
        // The demo's own player stands at (60,10,45), clearly centred and well-framed by the
        // demo camera (60,2,52) -- reusing that spot avoids placing a huge scaled mesh so close
        // to camera that the view clips into one of its faces (a flat, textureless fill with no
        // sway-vs-no-sway distinction, which is what (60,6,48) at 10x scale produced).
        pos["x"] = 60.0; pos["y"] = 10.0; pos["z"] = 45.0;
        xform["position"] = pos;
        fkyaml::node scale = fkyaml::node::mapping();
        scale["x"] = 3.0; scale["y"] = 3.0; scale["z"] = 3.0;
        xform["scale"] = scale;
        fkyaml::node mr = fkyaml::node::mapping();
        mr["type"] = std::string("MeshRenderer");
        mr["mesh_path"] = std::string("nature/grass_tuft");
        fkyaml::node mat = fkyaml::node::mapping();
        mat["shader"] = std::string("foliage");
        fkyaml::node params = fkyaml::node::sequence();
        params.as_seq().push_back(fkyaml::node(1.0));  // flexibility
        params.as_seq().push_back(fkyaml::node(1.5));  // frequency (rad/s)
        params.as_seq().push_back(fkyaml::node(2.0));  // height (mesh units)
        params.as_seq().push_back(fkyaml::node(0.0));  // up axis: +Z
        mat["shader_params"] = params;
        mat["alpha_mode"] = std::string("CUTOUT");
        mat["alpha_cutoff"] = 0.4;
        mr["material"] = mat;
        fkyaml::node comps = fkyaml::node::sequence();
        comps.as_seq().push_back(xform);
        comps.as_seq().push_back(mr);
        obj["components"] = comps;
        obj["children"] = fkyaml::node::sequence();
        doc["scene"]["root_objects"].as_seq().push_back(obj);
    });
}

/** @brief One capture after `settle_ticks` frames with the sun held still and the
 *         frame-varying stylization off, so runs differ only by what the wind moved, and with
 *         the demo's CameraController pinned to a fixed orbit target instead of following its
 *         authored `tracker: player` -- the established pattern for a
 *         deterministic camera (see temporal_reprojection_test.cpp's ssao_follows_a_moving_object).
 *         Orbit mode re-derives yaw/pitch from the seed Transform relative to `target` at
 *         start(): with the default target (world origin) and this scene's explicit `distance:
 *         10`, a missing or unexpected tracker relocates the camera to ~10 units from ORIGIN
 *         along that seed direction -- nowhere near the terrain's playable area -- which is
 *         what an earlier version of this fixture hit after removing the tracked "player" object
 *         outright. Pinning the target directly sidesteps tracker resolution entirely. */
Frame pinned_capture(const std::string& scene, const glm::vec3& target, const char* debug_view = "",
                     uint32_t settle_ticks = 112) {
    toy::world::TerraStore::instance().clear();
    toy::core::Engine engine(make_test_config(scene, 320, 180, 320, 180));
    if (auto* cc = engine.scene().find_first_component<toy::scene::CameraController>()) {
        cc->tracker            = "";
        cc->target             = target;
        cc->target_offset      = glm::vec3(0.0f);
        cc->distance           = 6.0f;
        cc->pitch_deg          = 20.0f;
        cc->yaw_deg            = 0.0f;
        cc->follow_smoothing   = 0.0f;
        cc->movement_smoothing = 0.0f;
    }
    tick_frames(engine, 8);
    if (auto* w = engine.weather()) w->set_clock_paused(true);
    auto& rc = engine.render_config();
    rc.dither_enabled = false;
    rc.palette_enabled = false;
    rc.outline_enabled = false;
    if (debug_view[0] != '\0') rc.debug_view = debug_view;
    tick_frames(engine, settle_ticks);
    Frame f = engine.capture_image();
    toy::world::TerraStore::instance().clear();
    return f;
}

/** @brief Changed pixels (by more than `threshold`, max over RGB) anywhere in two same-frame
 *         captures. The whole frame, not a "ground" crop: a tall test object's swaying TIP
 *         (bend grows with height fraction squared, foliage_surface.glsl) rises well above the
 *         lower rows, leaving only its near-static root inside such a crop. */
long long full_diff(const Frame& a, const Frame& b, int threshold = 1) {
    if (!same_extent(a, b)) return -1;
    long long n = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            const std::size_t k = (static_cast<std::size_t>(y) * a.width + x) * a.channels;
            int d = 0;
            for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(int(a.pixels[k + c]) - int(b.pixels[k + c])));
            n += d > threshold;
        }
    }
    return n;
}

} // namespace

/**
 * @brief Foliage bends in the weather's wind, in the LIT image: at the same frame a windy run
 *        differs from a calm one, two calm runs are identical, and the wind reaches the surface
 *        UBO.
 *
 * Measured on `velocity_test_scene`'s planted tuft through `pinned_capture`, the same controlled
 * framing the two checks below use, and with `full_diff` for the same reason they do -- the
 * plant's swaying tip rises above the lower "ground" rows. The demo's OWN scattered
 * grass cannot carry this check at the demo camera's framing: at 0.6 m tall and tens of metres
 * out its sway is sub-pixel, and the lit-image difference it produces is around 60 pixels
 * whatever the wind (16 at 1.5 m/s, 62 at 12 -- real, scaling with wind, and far below anything
 * a threshold can separate from noise). See velocity_test_scene's own note.
 */
COOPA_TEST(foliage_sways_in_the_weather_wind) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::string calm_scene = velocity_test_scene("heightmap_terrain_calm", 0.0f);
    const std::string windy_scene = velocity_test_scene("heightmap_terrain_windy", 12.0f);
    const glm::vec3 plant(60.0f, 10.0f, 46.0f);
    {
        toy::world::TerraStore::instance().clear();
        toy::core::Engine engine(make_test_config(windy_scene, 160, 90, 160, 90));
        tick_frames(engine, 30);
        expect(glm::length(glm::vec2(engine.pipeline().surface_world_ubo().wind)) > 8.0f,
               "foliage: the windy condition's wind reaches the surface UBO");
    }
    const Frame calm_a = pinned_capture(calm_scene, plant);
    const Frame calm_b = pinned_capture(calm_scene, plant);
    const Frame windy = pinned_capture(windy_scene, plant);
    // The demo's own clear weather is a light 1.5 m/s breeze: it must visibly stir foliage too,
    // which at this framing it does an order of magnitude over the noise floor rather than the
    // margin a 12 m/s gale gets. Measured: 0 pixels calm/calm, 154 at 1.5 m/s, 2100 at 12.
    const Frame breeze = pinned_capture(velocity_test_scene("heightmap_terrain_breeze", 1.5f), plant);
    const long long same = full_diff(calm_a, calm_b, 10);
    const long long swayed = full_diff(calm_a, windy, 10);
    const long long stirred = full_diff(calm_a, breeze, 10);
    std::printf("    foliage sway: calm/calm %lld, calm/windy %lld, calm/breeze %lld\n",
                same, swayed, stirred);
    expect(same < 50, "foliage: two calm runs agree (" + std::to_string(same) + " pixels)");
    expect(swayed > 500, "foliage: the wind moves the foliage (" + std::to_string(swayed) + " pixels differ from calm)");
    expect(stirred > 100, "foliage: a light breeze stirs it visibly too (" + std::to_string(stirred) + " pixels)");
    if (swayed <= 500 || stirred <= 100) {
        dump_frame(calm_a, "calm");
        dump_frame(windy, "windy");
        dump_frame(breeze, "breeze");
    }
}

/** @brief Wind-swayed foliage writes a TRUE motion vector, not the zero one a G-buffer that
 *         cancels the hook's own displacement out of frag_prev_world_pos would produce (TAA then
 *         ghosts it: see gfx/surface/gbuffer_vs.glsl's GFX_SURFACE_PREV_POSE). A calm run's
 *         velocity channel is the exact-zero baseline a windy run must differ from over the
 *         plant, since nothing else in the frame moves (static camera, sun, terrain). */
COOPA_TEST(foliage_carries_a_real_motion_vector) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::string calm_scene = velocity_test_scene("heightmap_terrain_velocity_calm", 0.0f);
    const std::string windy_scene = velocity_test_scene("heightmap_terrain_velocity_windy", 12.0f);
    const glm::vec3 plant(60.0f, 10.0f, 46.0f);
    const Frame velocity_calm_a = pinned_capture(calm_scene, plant, "velocity");
    const Frame velocity_calm_b = pinned_capture(calm_scene, plant, "velocity");
    const Frame velocity_windy = pinned_capture(windy_scene, plant, "velocity");
    // The test plant is scaled tall enough that its swaying tip rises above the lower
    // "ground" rows, leaving only its near-static root (bend ~ 0 there) in a cropped
    // comparison. A per-frame velocity also encodes a DELTA, not a swing's full
    // amplitude, so this needs a much smaller threshold than the lit-image checks above.
    const long long same = full_diff(velocity_calm_a, velocity_calm_b, 1);
    const long long moved = full_diff(velocity_calm_a, velocity_windy, 1);
    // Proves it is the HOOK producing the signal, not camera jitter or capture noise: two calm
    // runs must agree, exactly as the lit-image check above does.
    expect(same < 50, "velocity: two calm runs agree (" + std::to_string(same) + " pixels)");
    // Before GFX_SURFACE_PREV_POSE this was 0: frag_prev_world_pos cancelled the hook's own
    // displacement out of the velocity attachment, so a windy run's velocity channel was
    // identical to a calm one's over the swaying plant.
    expect(moved > 300, "velocity: the wind writes a non-zero motion vector (" + std::to_string(moved) + " pixels differ from calm)");
    if (moved <= 300) {
        dump_frame(velocity_calm_a, "velocity_calm");
        dump_frame(velocity_windy, "velocity_windy");
    }
}

/** @brief A windy scene must not report itself still: wind-animated surfaces (foliage, hair)
 *         move without their matrix moving, so pipeline_gather.cpp's wind_animated() keeps
 *         scene_moved_ true while real wind reaches one. Observed through the temporal freeze's
 *         effect on SSAO -- record_scene_()'s stillness block holds every stochastic resolve
 *         verbatim once the scene has been still for kTemporalFreezeAfter frames. With the
 *         camera pinned dead still, a CALM scene's SSAO channel settles and freezes (adjacent
 *         frames agree); a WINDY one keeps resolving (adjacent frames differ) because the
 *         swaying plant keeps scene_moved_ true. */
COOPA_TEST(windy_scene_does_not_freeze_the_temporal_resolves) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::string calm_scene = velocity_test_scene("heightmap_terrain_freeze_calm", 0.0f);
    const std::string windy_scene = velocity_test_scene("heightmap_terrain_freeze_windy", 12.0f);
    const glm::vec3 plant(60.0f, 10.0f, 46.0f);
    // Two adjacent frames (settle_ticks N and N+1) of the SAME deterministic run: if the
    // temporal freeze is holding, the SSAO channel is byte-identical between them.
    const Frame calm_n   = pinned_capture(calm_scene, plant, "ssao", 112);
    const Frame calm_n1  = pinned_capture(calm_scene, plant, "ssao", 113);
    const Frame windy_n  = pinned_capture(windy_scene, plant, "ssao", 112);
    const Frame windy_n1 = pinned_capture(windy_scene, plant, "ssao", 113);
    const long long calm_delta = full_diff(calm_n, calm_n1, 1);
    const long long windy_delta = full_diff(windy_n, windy_n1, 1);
    expect(calm_delta < 10, "freeze: a calm scene's SSAO is byte-static once still (" +
                                std::to_string(calm_delta) + " pixels differ)");
    expect(windy_delta > 50, "freeze: a windy scene's SSAO keeps resolving (" +
                                 std::to_string(windy_delta) + " pixels differ)");
    if (calm_delta >= 10 || windy_delta <= 50) {
        dump_frame(calm_n, "freeze_calm_n");
        dump_frame(calm_n1, "freeze_calm_n1");
        dump_frame(windy_n, "freeze_windy_n");
        dump_frame(windy_n1, "freeze_windy_n1");
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
    toy::world::TerraStore::instance().clear();
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
    toy::world::TerraStore::instance().clear();
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

    const float chunk_size = static_cast<float>(t->field.geometry().chunk_tiles) * t->field.geometry().tile_size;
    const float height = t->field.geometry().height;
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
