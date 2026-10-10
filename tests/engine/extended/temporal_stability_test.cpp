/**
 * @file temporal_stability_test.cpp
 * @brief Temporal image-quality contracts with tuned thresholds (extended tier: run when working on
 *        TAA, the SSAO/SSR resolves or the cloud reconstruction):
 *        - a static camera over static terrain renders a static image (the TAA orbit flicker);
 *        - the image settles within a few frames after the camera stops (the SSAO resolve tail) --
 *          known to FAIL at the time of the test restructure (2026-10), kept failing on purpose;
 *        - the cloud layer is stable frame to frame with and without TAA (the shimmering sky).
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("temporal_stability");

using namespace toy::test;

/**
 * @brief A static camera over static geometry must render a STATIC image.
 *
 * The regression test for a flicker that shipped unnoticed: with `aa_mode: taa`, the 8-frame
 * Halton jitter makes the whole render exactly 8-periodic, and any resolve that filters it
 * with a plain exponential blend converges that periodic input to a periodic ORBIT rather than
 * a fixed point -- on terrain_test's block faces the surviving orbit reads as boiling. TAA's
 * age-weighted accumulation (a true running average at rest, with a quantisation floor on the
 * variance clip so flat regions keep their age) is what this test holds to the contract; the
 * threshold below fails on any re-introduced orbit.
 *
 * Two things make this test able to see what the existing suite could not:
 *
 *   - It renders with the SHIPPED config (make_shipped_config), so whatever `aa_mode` actually
 *     ships is what gets exercised. make_test_config()'s defaults have AA off entirely.
 *   - It compares ADJACENT frames. render_pixel's drift assertion spaces its captures by
 *     kNoiseCycle -- a whole number of cycles -- which lands on the same jitter phase and is
 *     therefore structurally blind to a phase-periodic orbit. Comparing neighbours is the whole
 *     point; a cycle is invisible to any test sampling it stroboscopically.
 */
COOPA_TEST(static_camera_converges_to_a_static_image) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_shipped_config("assets/scenes/terrain_test/scene.yaml", 320, 180));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr, "converge: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr) return;
    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    expect(terrain != nullptr, "converge: the scene has a Terrain component");
    if (terrain == nullptr) return;

    // Same shrink the streaming test uses -- a 32-cell map in nine 8x8 chunks, not the demo's
    // ~10k cells, so generation and meshing finish in a second rather than most of a minute.
    terrain->grid_size                  = 32;
    terrain->params.tiles_per_grid_unit = 2;
    terrain->params.chunk_size          = 8;
    terrain->params.view_radius         = 1;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 9; });
    tick_frames(engine, 80); // let every temporal filter settle; nothing moves from here on

    std::vector<Frame> frames;
    for (int i = 0; i < 18; ++i) {
        engine.tick();
        frames.push_back(engine.capture_image(/*low_res=*/true));
    }

    auto lag_mean = [&](int lag) {
        double acc = 0.0;
        int n = 0;
        for (size_t i = lag; i < frames.size(); ++i) {
            acc += mean_abs_delta(frames[i], frames[i - static_cast<size_t>(lag)]);
            ++n;
        }
        return n > 0 ? acc / n : 0.0;
    };

    // Budget in 0-255 levels per pixel, averaged over the frame. A converged renderer measures
    // exactly 0.0000 here (smaa and off both do); taa measured 0.0086. The threshold sits an
    // order of magnitude below that so it fails on a re-introduced orbit, while leaving room for
    // a genuinely dithered effect to contribute a texel or two.
    const double lag1 = lag_mean(1);
    expect(lag1 < 0.002, "converge: a static camera renders a static image");
    if (lag1 >= 0.002) std::cerr << "         adjacent-frame delta " << lag1 << " levels/px\n";

    // On failure, print the lag profile -- it does not just say the image moves, it says WHY.
    // A periodic orbit collapses to ~0 at a whole cycle while staying high at every other lag,
    // so `1=0.006 2=0.008 4=0.009 8=0` names the cycle length in the output. (Deliberately not
    // its own expect(): under the bug lag 8 is ~0, so any "lag8 must be small" assertion would
    // PASS on exactly the case it is meant to catch. lag 1 is the assertion; this is evidence.)
    if (lag1 >= 0.002) {
        std::cerr << "         lag profile: 1=" << lag_mean(1) << " 2=" << lag_mean(2)
                  << " 4=" << lag_mean(4) << " 8=" << lag_mean(8) << "\n";
    }
}

/**
 * @brief After the camera stops, the image must stop too -- within a couple of frames.
 *
 * The regression test for the second flicker: SSAO's temporal resolve reprojects AO history and
 * rejects it on only off-screen and behind-eye, with no depth/disocclusion test. Over blocky
 * terrain, camera motion rejects history across every depth discontinuity at once, exposing raw
 * 4x4-tile AO noise -- and the accumulator then refills at ssao_temporal_blend, so the image
 * keeps changing for dozens of frames after the camera has stopped. See config.yaml's
 * `ssao_temporal_enabled` for the measurement that pinned it.
 *
 * Three conditions are load-bearing and were each established by measurement:
 *
 *   - **Full render resolution.** SSAO's noise is a screen-locked 4x4 TEXEL tile, so it averages
 *     away at a small render size: the identical trajectory at 320x180 settles in 0 frames even
 *     with the bug present. A cheap low-res version of this test would pass on a broken build.
 *   - **A close camera.** Rotating close to the terrain is what produces the disocclusion the
 *     resolve mishandles; from far away the parallax is too small to reject much history.
 *   - **Camera smoothing zeroed**, so the camera stops dead the frame the sweep ends. Otherwise
 *     CameraController's own exponential chase is measured as a renderer tail.
 */
COOPA_TEST(image_settles_after_camera_stops) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_shipped_config("assets/scenes/terrain_test/scene.yaml", 1920, 1080);
    // Eye adaptation is deliberately excluded. It re-meters after the rotation and then eases
    // exposure toward the new target at auto_exposure_speed_up/down -- a slow, intended,
    // global brightness glide, not a renderer failing to settle. Left on (config.yaml ships it
    // on), it alone keeps the per-frame delta far above the 0.03 threshold for the whole window,
    // masking the temporal-resolve tail this test exists to catch.
    config.render.auto_exposure_enabled = false;
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
           "settle: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;

    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    expect(terrain != nullptr && controller != nullptr,
           "settle: the scene has a Terrain and a CameraController");
    if (terrain == nullptr || controller == nullptr) return;

    shrink_terrain_and_centre(*terrain, *camera->scene);

    controller->follow_smoothing   = 0.0f;  // stop dead, so the tail measured is the renderer's
    controller->movement_smoothing = 0.0f;
    controller->distance           = 18.0f; // close in: this is what disoccludes under rotation
    controller->pitch_deg          = 18.0f;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
    tick_frames(engine, 90);

    for (int i = 0; i < 24; ++i) {  // rotate...
        controller->yaw_deg += 1.5f;
        engine.tick();
    }
    // ...and stop. Everything after this point is the renderer failing to settle.

    Frame prev = engine.capture_image(/*low_res=*/true);
    std::vector<double> curve;
    int settled_at = -1;
    for (int i = 0; i < 40; ++i) {
        engine.tick();
        Frame current = engine.capture_image(true);
        const double delta = mean_abs_delta(current, prev);
        curve.push_back(delta);
        if (settled_at < 0 && delta < 0.03) settled_at = i;
        prev = std::move(current);
    }

    // What this guards is SSAO's temporal resolve, whose tail is ~10x everything else's: with it
    // enabled the curve starts near 0.45 and is still above 0.13 forty frames later, while the
    // shipped configuration is under 0.03 within a handful of frames. The threshold sits between
    // those rather than at zero, because SSR's own temporal resolve leaves a real residual tail
    // (~0.047, decaying) that this test deliberately does not fail on -- turning SSR off is the
    // only thing that reaches 0.0000, and SSR earns its keep on other scenes.
    expect(settled_at >= 0 && settled_at <= 8, "settle: the image stops when the camera stops");
    if (settled_at < 0 || settled_at > 8) {
        std::cerr << "         settled after " << settled_at << " frames; decay:";
        for (size_t i = 0; i < curve.size() && i < 10; ++i) std::cerr << " " << curve[i];
        std::cerr << "\n";
    }
}

/**
 * @brief The cloud layer is temporally stable: a still camera over still clouds (FIXED_DT=0, no
 *        drift) under TAA gives consecutive frames that agree. The clouds' own reconstruction
 *        (SkyCloudPass) converges the march's per-frame jitter; the old per-frame march left
 *        rotating dither noise TAA could not settle -- the "shimmering" sky.
 */
COOPA_TEST(sky_clouds_are_temporally_stable) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/tests/rendering/sky_test/scene.yaml", 480, 270, 480, 270);
    config.render.aa_mode = "taa";
    config.render.auto_exposure_enabled = false;
    config.render.bloom_enabled = false;
    config.render.shadows_enabled = false;
    toy::core::Engine engine(std::move(config));
    toy::weather::WeatherSystem* w = engine.weather();
    expect(w != nullptr, "cloud stability: sky_test has its weather");
    if (!w) return;
    auto& pl = engine.pipeline();
    auto& cfg = pl.render_config_mut();
    cfg.shadows_enabled = false;
    cfg.outline_enabled = false;
    w->set_time(15.0f);
    w->set_condition("cloudy", 0.0f);
    // Look up into the clouds, so most of the frame is cloud layer.
    coopa::scene::SceneObject* cam = engine.scene().find_object("camera");
    if (cam && cam->get_transform()) cam->get_transform()->transform().set_rotation(glm::vec3(130.0f, 0.0f, 90.0f));
    tick_frames(engine, 90);
    expect(pl.render_config().clouds && pl.physical_sky_active(), "cloud stability: the clouds are on");
    const Frame a = engine.capture_image(true);
    tick_frames(engine, 1);
    const Frame b = engine.capture_image(true);
    expect(black_block_pixels(a) == 0, "cloud stability: no black (NaN) blocks");
    const long long flicker = count_diff(a, b, 3);
    const long long limit = long(a.width * a.height) / 200;   // 0.5% of the frame
    if (flicker > limit) { dump_frame(a, "cloud_stable_a"); dump_frame(b, "cloud_stable_b"); }
    expect(flicker <= limit, "cloud stability: consecutive frames agree (" + std::to_string(flicker) +
                                 " px differ by more than 3 levels, limit " + std::to_string(limit) + ")");

    // Without TAA (FXAA, the shipped default) and at the lowest tier, nothing smooths the
    // clouds after their own reconstruction: its anti-flicker cap alone must hold them still.
    // (aa_mode is startup-fixed: a second engine.)
    toy::core::AppConfig low_config = make_test_config("assets/scenes/tests/rendering/sky_test/scene.yaml", 480, 270, 480, 270);
    low_config.render.aa_mode = "fxaa";
    low_config.render.cloud_quality = toy::render::RenderQuality::Low;
    low_config.render.auto_exposure_enabled = false;
    low_config.render.bloom_enabled = false;
    low_config.render.shadows_enabled = false;
    toy::core::Engine low(std::move(low_config));
    if (toy::weather::WeatherSystem* lw = low.weather()) {
        lw->set_time(15.0f);
        lw->set_condition("cloudy", 0.0f);
    }
    low.pipeline().render_config_mut().shadows_enabled = false;
    low.pipeline().render_config_mut().cloud_quality = toy::render::RenderQuality::Low;
    if (coopa::scene::SceneObject* lc = low.scene().find_object("camera"); lc && lc->get_transform())
        lc->get_transform()->transform().set_rotation(glm::vec3(130.0f, 0.0f, 90.0f));
    tick_frames(low, 120);
    expect(low.pipeline().render_config().aa_mode == "fxaa", "cloud stability: the second engine runs without TAA");
    const Frame c = low.capture_image(true);
    tick_frames(low, 1);
    const Frame d = low.capture_image(true);
    const long long flicker_low = count_diff(c, d, 2);
    const long long limit_low = long(c.width * c.height) / 1000;   // 0.1% of the frame
    if (flicker_low > limit_low) { dump_frame(c, "cloud_stable_low_a"); dump_frame(d, "cloud_stable_low_b"); }
    expect(flicker_low <= limit_low, "cloud stability: no TAA, low tier, frames agree (" + std::to_string(flicker_low) +
                                         " px differ by more than 2 levels, limit " + std::to_string(limit_low) + ")");
}

/**
 * @brief Low volumetric clouds seen from above (topdown_sky_test) are as stable as the sky's: a
 *        still orbit camera over still clouds gives consecutive frames that agree, with TAA and,
 *        at the lowest tier, without it. Their march now ends at the ground and their layer is
 *        composited over geometry (cloud_composite.frag), so this is a different path from the
 *        sky's -- and their shadows come from a map rebuilt every frame, which must not shimmer
 *        either.
 */
COOPA_TEST(topdown_clouds_are_temporally_stable) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    for (const bool taa : {true, false}) {
        toy::core::AppConfig config = make_test_config("assets/scenes/tests/rendering/topdown_sky_test/scene.yaml", 480, 270, 480, 270);
        config.render.aa_mode = taa ? "taa" : "fxaa";
        config.render.cloud_quality = taa ? toy::render::RenderQuality::High : toy::render::RenderQuality::Low;
        config.render.auto_exposure_enabled = false;
        config.render.bloom_enabled = false;
        config.render.shadows_enabled = false;
        toy::core::Engine engine(std::move(config));
        auto& cfg = engine.pipeline().render_config_mut();
        cfg.shadows_enabled = false;
        cfg.outline_enabled = false;
        cfg.cloud_quality = taa ? toy::render::RenderQuality::High : toy::render::RenderQuality::Low;
        if (toy::weather::WeatherSystem* w = engine.weather()) {
            w->set_time(11.0f);
            w->set_condition("cloudy", 0.0f);
        }
        tick_frames(engine, 120);
        const std::string tag = taa ? "taa" : "no taa, low tier";
        expect(engine.pipeline().volumetric_clouds_traced() && engine.pipeline().cloud_shadows_active(),
               "topdown stability (" + tag + "): the low clouds and their shadows are on");
        const Frame a = engine.capture_image(true);
        tick_frames(engine, 1);
        const Frame b = engine.capture_image(true);
        expect(black_block_pixels(a) == 0, "topdown stability (" + tag + "): no black (NaN) blocks");
        const long long flicker = count_diff(a, b, taa ? 3 : 2);
        const long long limit = long(a.width * a.height) / (taa ? 200 : 1000);
        if (flicker > limit) { dump_frame(a, "topdown_stable_a"); dump_frame(b, "topdown_stable_b"); }
        expect(flicker <= limit, "topdown stability (" + tag + "): consecutive frames agree (" + std::to_string(flicker) +
                                     " px differ, limit " + std::to_string(limit) + ")");
    }
}
