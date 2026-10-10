/**
 * @file water_scene_test.cpp
 * @brief water_test and underwater_test end to end: lake and river bake with physics, floaters
 *        float and sink, the river delivers its crate, ripples reach the renderer, every quality tier
 *        re-bakes and draws, the underwater pass engages and disengages, and the shader animates on
 *        the buoyancy clock (a past out-of-phase bug).
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/water_fixtures.h"

COOPA_TEST_SUITE("water_scene");

using namespace toy::test;

namespace {

/** @brief Mean RGB of a capture. */
glm::vec3 mean_rgb(const Frame& f) {
    glm::dvec3 sum(0.0);
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* px = &f.pixels[i * f.channels];
        sum += glm::dvec3(px[0], px[1], px[2]);
    }
    return glm::vec3(sum / static_cast<double>(std::max<size_t>(pixels, 1)));
}

} // namespace

/** @brief The whole scene: water draws, floaters float, the river delivers its crates -- then
 *         water_quality switches live through every tier on the same Engine. */
COOPA_TEST(water_scene_simulates_renders_and_switches_quality_tiers) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/water_test/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));

    tick_frames(engine, 3);
    auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
    expect(water != nullptr, "water scene: Engine installed the WaterSystem");
    std::size_t baked = 0;
    for (auto* b : engine.scene().get_components<toy::water::WaterBody>()) baked += b->bake_stage == 2 ? 1 : 0;
    expect(baked == 2u, "water scene: lake and river both baked with physics (depth + obstacles)");

    auto river_s = [&]() {
        auto* o = engine.scene().find_object("river_crate_a");
        return o ? com_of(o) : glm::vec3(0.0f);
    };
    const glm::vec3 crate_start = river_s();
    tick_frames(engine, 600);
    const glm::vec3 crate_end = river_s();
    expect(crate_end.z < crate_start.z - 4.0f, "water scene: the river crate rode the rapids down the hill");
    expect(glm::length(glm::vec2(crate_end) - glm::vec2(crate_start)) > 12.0f,
           "water scene: ...a long way downstream");

    auto* stone = engine.scene().find_object("stone");
    auto* light = engine.scene().find_object("crate_light");
    expect(stone && com_of(stone).z < -2.0f, "water scene: the stone sank to the bed");
    if (light) {
        auto* b = light->get_component<toy::water::Buoyancy>();
        expect(b->in_water && b->submerged_fraction > 0.15f && b->submerged_fraction < 0.6f,
               "water scene: the light crate floats (submerged " + std::to_string(b->submerged_fraction) + ")");
    }

    expect(!engine.pipeline().water_state().ripples.empty(),
           "water scene: ripple rings (boat wake, bobbing floaters) reach the renderer");
    expect(!engine.pipeline().underwater_active(), "water scene: the overview camera is not underwater");

    const Frame frame = engine.capture_image(/*low_res=*/true);
    long long watery = 0;
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* px = &frame.pixels[i * frame.channels];
        if (px[2] > px[0] + 40 && px[2] > 90) ++watery; // distinctly blue
    }
    expect(watery > static_cast<long long>(pixels / 20), "water scene: the lake is on screen and blue");
    if (watery <= static_cast<long long>(pixels / 20)) dump_frame(frame, "water_scene");

    // water_quality switches live: every tier re-bakes and draws the lake (as several LOD'd
    // tiles), Low at a coarser grid than High, with the tier's shader detail handed over.
    {
        using toy::render::RenderQuality;
        std::size_t high_verts = 0, low_verts = 0;
        for (RenderQuality q : {RenderQuality::High, RenderQuality::Low, RenderQuality::Medium, RenderQuality::Ultra}) {
            engine.render_config().water_quality = q;
            tick_frames(engine, 4);
            const std::string tier = std::to_string(static_cast<int>(q));
            auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
            expect(water && static_cast<int>(water->settings().quality) == static_cast<int>(q),
                   "water tiers: the system runs tier " + tier);
            const toy::water::WaterBody* lake = nullptr;
            bool published = true;
            for (auto* b : engine.scene().get_components<toy::water::WaterBody>()) {
                published = published && toy::water::WaterSystem::is_published(*b);
                if (b->mode == toy::water::WaterMode::Planar) lake = b;
            }
            expect(published, "water tiers: every body published at tier " + tier);
            if (lake && q == RenderQuality::High) {
                high_verts = lake->query.vertices().size();
                expect(lake->tiles.size() > 1u, "water tiers: the 46 m lake is drawn as several tiles (" +
                                                    std::to_string(lake->tiles.size()) + ")");
            }
            if (lake && q == RenderQuality::Low) low_verts = lake->query.vertices().size();
            expect(engine.pipeline().water_state().ripple_layers == water->settings().ripple_layers &&
                       engine.pipeline().water_state().ripples.size() <= water->settings().max_ripples,
                   "water tiers: the tier's shader detail and ring cap reach the renderer");

            const Frame frame = engine.capture_image(/*low_res=*/true);
            long long watery = 0;
            const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
            for (size_t i = 0; i < pixels; ++i) {
                const uint8_t* px = &frame.pixels[i * frame.channels];
                if (px[2] > px[0] + 40 && px[2] > 90) ++watery;
            }
            expect(watery > static_cast<long long>(pixels / 20), "water tiers: the lake draws at tier " + tier);
            if (watery <= static_cast<long long>(pixels / 20)) dump_frame(frame, "water_tier_" + tier);
        }
        expect(low_verts > 0 && low_verts < high_verts, "water tiers: low bakes a coarser grid than high (" +
                                                            std::to_string(low_verts) + " vs " + std::to_string(high_verts) + ")");
    }
}


/** @brief The camera under a water surface: UnderwaterPass engages, the frame turns to water
 *         (red absorbed first), and rising above the surface turns it back off. */
COOPA_TEST(underwater_pass_engages_below_the_surface) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/underwater_test/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 60);

    expect(engine.pipeline().underwater_active(), "underwater scene: the camera starts below the surface");
    const Frame below = engine.capture_image(/*low_res=*/true);
    const glm::vec3 c_below = mean_rgb(below);
    expect(c_below.g > c_below.r * 1.4f && c_below.b > c_below.r * 1.4f,
           "underwater scene: the frame is water-tinted, red absorbed (mean rgb " +
               std::to_string(c_below.r) + ", " + std::to_string(c_below.g) + ", " + std::to_string(c_below.b) + ")");

    // Rise above the surface.
    auto* cc = engine.scene().find_first_component<toy::scene::CameraController>();
    expect(cc != nullptr, "underwater scene: has an orbit camera");
    if (cc) {
        cc->movement_smoothing = 0.0f;
        cc->distance = 30.0f;
        cc->pitch_deg = 45.0f;
    }
    tick_frames(engine, 30);
    expect(!engine.pipeline().underwater_active(), "underwater scene: above the surface the pass is off");
    const Frame above = engine.capture_image(true);
    const glm::vec3 c_above = mean_rgb(above);
    expect(glm::length(c_above - c_below) > 15.0f, "underwater scene: ...and the frame changes accordingly");
    if (!(c_below.g > c_below.r * 1.4f)) dump_frame(below, "underwater_below");
}

/**
 * @brief The water shader animates waves on the water system's clock, not the renderer's.
 *
 * Buoyancy evaluates the waves on the CPU at WaterSystem::time(), which starts with the scene;
 * the renderer's own clock starts with the pipeline. Before the two were tied, the drawn waves
 * and the ones floaters rode were at unrelated phases (any time spent before the scene loaded,
 * e.g. the editor before Play, was the offset), so bodies bobbed out of step with the surface.
 * Here the renderer has already run frames before the scene loads, so the clocks differ, and the
 * time handed to the shader must be the water clock (less the interpolation lag, < one step).
 */
COOPA_TEST(shader_shares_the_buoyancy_clock) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 30);   // the renderer's clock runs ahead of any later scene's
    engine.load_scene("assets/scenes/water_test/scene.yaml");
    tick_frames(engine, 20);
    auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
    expect(water != nullptr, "water clock: water_test has a water system");
    if (!water) return;
    const float handed = engine.pipeline().water_state().time;
    expect(handed >= 0.0f && handed <= water->time() && water->time() - handed <= 1.0f / 60.0f + 1e-4f,
           "water clock: the shader gets the water system's time (" + std::to_string(handed) +
           " vs " + std::to_string(water->time()) + ")");
    expect(water->time() < 0.5f + 20.0f / 60.0f,
           "water clock: the water clock started with the scene (" + std::to_string(water->time()) + " s)");
}
