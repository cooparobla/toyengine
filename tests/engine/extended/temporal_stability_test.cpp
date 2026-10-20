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
#include <cstdlib>
#include <utility>
#include <filesystem>
#include <fstream>
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
 * a fixed point -- on terrain_demo's block faces the surviving orbit reads as boiling. TAA's
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

    toy::core::Engine engine(make_shipped_config(voxel_terrain_demo(), 320, 180));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr, "converge: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr) return;
    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    expect(terrain != nullptr, "converge: the scene has a Terrain component");
    if (terrain == nullptr) return;

    // Same shrink the streaming test uses -- a 32-cell map in nine 8x8 chunks, not the demo's
    // ~10k cells, so generation and meshing finish in a second rather than most of a minute.
    terrain->generator.grid_size        = 32;
    terrain->params.tiles_per_grid_unit = 2;
    terrain->params.chunk_size          = 8;
    terrain->params.view_radius         = 1;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 9; });
    // Settled, not merely warmed: TAA's accumulation is a true running average at rest (see
    // taa_resolve), so its per-frame change decays as 1/N rather than exponentially, and what
    // this test measures is that decay's tail. Measured here at 320x180 -- 80 ticks: 0.0038
    // levels/px, 200: 0.0021, 400: under the 0.002 budget, 800: no better. The 80 this test was
    // written with predates the age-weighted resolve; a shorter settle measures the tail, not
    // the orbit the test is for. Attributed by elimination: with every stochastic pass disabled
    // (SSAO, SSR, SSGI, contact shadows, volumetrics) the number is unchanged, and with AA off
    // it is worse (0.0089) -- it is the resolve converging, not noise leaking through it.
    tick_frames(engine, 480); // nothing in the scene moves from here on

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
        make_shipped_config(voxel_terrain_demo(), 1920, 1080);
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
    toy::core::AppConfig config = make_test_config("assets/scenes/rendering/sky_demo/scene.yaml", 480, 270, 480, 270);
    config.render.aa_mode = "taa";
    config.render.auto_exposure_enabled = false;
    config.render.bloom_enabled = false;
    config.render.shadows_enabled = false;
    toy::core::Engine engine(std::move(config));
    toy::weather::WeatherSystem* w = engine.weather();
    expect(w != nullptr, "cloud stability: sky_demo has its weather");
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
    toy::core::AppConfig low_config = make_test_config("assets/scenes/rendering/sky_demo/scene.yaml", 480, 270, 480, 270);
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
 * @brief Low volumetric clouds seen from above (clouds_demo) are as stable as the sky's: a
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
        toy::core::AppConfig config = make_test_config("assets/scenes/rendering/clouds_demo/scene.yaml", 480, 270, 480, 270);
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

/// Comma-separated pass names in `TOY_FLICKER_OFF` turn one suspect off for the measurement
/// below, so one binary runs the whole attribution matrix: `ssgi`, `ssr`, `contact`, `ssao`,
/// `volumetrics`. Every one of these is STARTUP-FIXED, so they are applied to the AppConfig
/// before the Engine is constructed, not to the live pipeline.
static std::string apply_flicker_overrides(toy::render::ToyRenderConfig& render) {
    const char* raw = std::getenv("TOY_FLICKER_OFF");
    if (raw == nullptr || *raw == '\0') return "none";
    std::string list(raw);
    // Exact token match on the comma-separated list, NOT substring: `find("ssr")` is also true
    // of "ssrjitter", so a substring test silently disabled SSR outright in every run meant to
    // probe one of its parameters, and four different probes came back byte-identical.
    std::vector<std::string> tokens;
    for (size_t b = 0, e; b <= list.size(); b = e + 1) {
        e = list.find(',', b);
        if (e == std::string::npos) e = list.size();
        if (e > b) tokens.push_back(list.substr(b, e - b));
    }
    auto has = [&](const char* name) {
        return std::find(tokens.begin(), tokens.end(), std::string(name)) != tokens.end();
    };
    if (has("ssgi"))        render.ssgi_traced           = false;
    if (has("ssr"))         render.ssr_enabled           = false;
    if (has("contact"))     render.contact_shadows_enabled = false;
    if (has("ssao"))        render.ssao_enabled          = false;
    if (has("volumetrics")) render.volumetrics_enabled   = false;
    if (has("ssrfull"))   render.ssr_half_res      = false;  // startup-fixed: must be set here
    if (has("shadows"))     render.shadows_enabled          = false;
    if (has("pcss"))        render.shadow_pcss_enabled      = false;
    if (has("hardshadows")) render.soft_shadows             = false;
    if (has("frustumfit"))  render.shadow_fit               = "frustum";
    if (has("fixedfocus"))  render.shadow_focus_distance    = 20.0f;  // pin the probed distance
    return list;
}

/**
 * @brief While the camera MOVES, the image must already be close to what it settles to.
 *
 * The complaint this measures: with `aa_mode` off, surfaces visibly flicker during camera
 * motion and go quiet a moment after it stops. `image_settles_after_camera_stops` above cannot
 * see that -- it starts measuring the frame the camera stops, so an artifact that lives only
 * DURING motion is invisible to it, and so is any fix for one.
 *
 * The metric is the thing itself: sweep the camera, capture the moving frame, hold the camera
 * dead still at that same pose until every temporal filter has converged, capture again, and
 * take the mean per-pixel distance between the two. A renderer whose moving image already looks
 * like its resting image scores near zero whatever its absolute look is; one that falls back to
 * a raw stochastic sample wherever motion invalidates history scores the amplitude of that
 * noise. Repeated over several poses so a single unlucky view cannot carry the number.
 *
 * `aa_mode` is forced off rather than left at whatever ships: TAA's own history blend hides
 * exactly this artifact, which is why it only became visible when AA was turned off.
 */
COOPA_TEST(moving_image_matches_its_converged_image) {
    // FIXED_DT=0, not a real step: the hold below has to converge the temporal filters WITHOUT
    // advancing the world, or the scene's own water, foliage and sky move during it and swamp
    // the measurement -- at 1/60 they contributed 2.24 of a 2.26 gap, i.e. essentially all of
    // it. Frame indices still advance with time stopped, which is what the stochastic passes
    // re-jitter on, so everything this test is about still happens.
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = make_shipped_config(voxel_terrain_demo(), 1920, 1080);
    // Same exclusion the settle test above makes, for the same reason: eye adaptation re-meters
    // after a rotation and glides exposure for a second afterwards, which is an intended global
    // change, not a renderer failing to track its own converged image.
    config.render.auto_exposure_enabled = false;
    config.render.aa_mode = "off";
    // `TOY_FLICKER_SHADOWS=cubeworld` swaps in the shadow block the game that reported this
    // artifact actually ships (cubeworld's assets/config.yaml), which is the ONLY part of its
    // render config that differs from toyengine's: PCSS on, the ultra tier's tap counts, a
    // 4096 map, and a focus fit that re-fits the cascade around a point 20 m from the camera
    // -- i.e. a shadow map whose texel grid is re-chosen as the camera moves. Everything the
    // screen-space toggles above cover is identical between the two configs.
    if (const char* preset = std::getenv("TOY_FLICKER_SHADOWS");
        preset != nullptr && std::string(preset) == "cubeworld") {
        config.render.shadow_quality             = toy::render::RenderQuality::Ultra;
        config.render.shadow_map_resolution      = 4096;
        config.render.shadow_distance            = 55.0f;
        config.render.shadow_fit                 = "focus";
        config.render.shadow_focus_radius        = 20.0f;
        config.render.shadow_pcf_max_texels      = 32.0f;
        config.render.shadow_pcss_enabled        = true;
        config.render.shadow_receiver_plane_bias = true;
        config.render.shadow_softness            = 0.35f;
    }
    const std::string disabled = apply_flicker_overrides(config.render);
    expect(config.render.debug_view == "off",
           "motion: the shipped config is rendering the image, not a debug channel");

    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
           "motion: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;

    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    expect(terrain != nullptr && controller != nullptr,
           "motion: the scene has a Terrain and a CameraController");
    if (terrain == nullptr || controller == nullptr) return;

    // `TOY_FLICKER_VIEW` points the measurement at one raw intermediate buffer instead of the
    // final image (debug_view is a runtime toggle, so it is set on the live pipeline). Measuring
    // the same gap per channel says WHERE the moving image departs from its converged one:
    // `albedo` is the G-buffer the surface shaders write, `ssgi`/`ssr`/`contact_shadows` are the
    // stochastic screen-space buffers, `direct` is the lighting that depends on none of them.
    if (const char* view = std::getenv("TOY_FLICKER_VIEW"); view != nullptr && *view != '\0') {
        engine.render_config().debug_view = view;
    }

    shrink_terrain_and_centre(*terrain, *camera->scene);
    controller->follow_smoothing   = 0.0f;  // the camera must stop dead, as in the settle test
    controller->movement_smoothing = 0.0f;
    controller->distance           = 18.0f;
    controller->pitch_deg          = 18.0f;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
    tick_frames(engine, 90);

    // Trajectory. `TOY_FLICKER_MOVE=fly` translates the orbit's target across the terrain
    // instead of (only) swinging the camera around it: rotation at a fixed radius reveals
    // little of what was hidden, while translation opens a fresh disocclusion behind every
    // block edge every frame, which is the case a reprojecting temporal filter has to handle
    // and the one a player flying over the world actually produces.
    const char* move_env = std::getenv("TOY_FLICKER_MOVE");
    const bool  fly      = move_env != nullptr && std::string(move_env) == "fly";
    const float fly_step = 0.35f;   // world units per frame; ~21 u/s at 60 Hz, a brisk sprint

    auto advance = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            if (fly) {
                if (auto* marker = camera->scene->find_object("focus_marker")) {
                    if (auto* tc = marker->get_transform()) {
                        glm::vec3 p = tc->transform().position();
                        tc->transform().set_position(p + glm::vec3(fly_step, 0.0f, 0.0f));
                    }
                }
            } else {
                controller->yaw_deg += 1.5f;
            }
            engine.tick();
        }
    };

    // Walk the whole path once before measuring: terrain streams chunks in around the camera,
    // and a chunk that arrives between the moving capture and the converged one is a difference
    // this test must not attribute to a temporal filter. The pre-pass gets them built, and the
    // marker is put back where it started afterwards.
    if (fly) {
        glm::vec3 start(0.0f);
        if (auto* marker = camera->scene->find_object("focus_marker"))
            if (auto* tc = marker->get_transform()) start = tc->transform().position();
        advance(4 * (10 + 1));
        if (auto* marker = camera->scene->find_object("focus_marker"))
            if (auto* tc = marker->get_transform()) tc->transform().set_position(start);
        tick_frames(engine, 60);
    }

    // Four legs. Ten frames of continuous motion before each capture puts the temporal filters
    // in their steady MOVING state first -- the first frame or two after a standing start still
    // carries the accumulation the camera built at rest.
    std::vector<double> gaps, still;
    Frame previous_pose;
    for (int pose = 0; pose < 4; ++pose) {
        advance(10);
        const Frame moving = engine.capture_image(/*low_res=*/true);
        tick_frames(engine, 60);   // the camera does not move again until the next pose
        const Frame converged = engine.capture_image(true);
        gaps.push_back(mean_abs_delta(moving, converged));

        // Sanity: the sweep must actually change the view. A trajectory that silently fails to
        // move the camera (a renamed follow target, a controller mode that ignores it) makes
        // every gap below tiny for the most misleading possible reason -- the test would report
        // a perfectly tracking renderer while measuring nothing at all.
        if (pose > 0) {
            const double travelled = mean_abs_delta(converged, previous_pose);
            expect(travelled > 1.0, "motion: leg " + std::to_string(pose) + " moved the view (" +
                                        std::to_string(travelled) + " levels/px)");
        }
        previous_pose = converged;

        // Control: a second hold of the same length with the camera still throughout. Anything
        // it measures is the renderer still not at rest (or the scene moving despite FIXED_DT=0)
        // rather than an effect of camera motion, so the artifact this test is about is the
        // first number MINUS this one. Keeping it is what makes the headline figure trustworthy
        // -- measured without it, scene animation alone read as a 2.24 renderer failure.
        tick_frames(engine, 60);
        still.push_back(mean_abs_delta(converged, engine.capture_image(true)));
    }

    double sum = 0.0, still_sum = 0.0;
    for (double g : gaps) sum += g;
    for (double g : still) still_sum += g;
    const double mean       = sum / double(gaps.size());
    const double still_mean = still_sum / double(still.size());
    const double excess     = mean - still_mean;   // the part motion is responsible for

    // Printed on every run, pass or fail: this test doubles as the attribution harness for the
    // flicker (TOY_FLICKER_OFF above), and a matrix of numbers is the whole output there.
    const char* view_env = std::getenv("TOY_FLICKER_VIEW");
    const char* shadow_env = std::getenv("TOY_FLICKER_SHADOWS");
    std::cerr << "         [motion gap] move=" << (fly ? "fly" : "yaw") << " shadows="
              << ((shadow_env != nullptr && *shadow_env != '\0') ? shadow_env : "toyengine")
              << " disabled=" << disabled
              << " view=" << ((view_env != nullptr && *view_env != '\0') ? view_env : "off")
              << " motion=" << mean << " still=" << still_mean << " excess=" << excess
              << " poses:";
    for (double g : gaps) std::cerr << " " << g;
    std::cerr << "\n";

    expect(excess < 0.60, "motion: the moving image tracks its converged image (" +
                              std::to_string(excess) + " levels/px above the scene's own motion)");
}

/// Pixels that sit INSIDE a flat surface: those whose 3x3 luma gradient in the supersampled
/// reference is below `threshold` levels. The reported artifact is on the open faces of cubes
/// and the terrain floor, not on their outlines, and a whole-frame mean is dominated by the
/// outlines -- every silhouette legitimately moves several levels per frame under any camera
/// motion, which buries a face-interior shimmer an order of magnitude smaller. Masking to the
/// interiors measures the thing being complained about.
static std::vector<uint8_t> flat_interior_mask(const Frame& f, int threshold = 6) {
    std::vector<uint8_t> mask(size_t(f.width) * f.height, 0);
    auto luma = [&](uint32_t x, uint32_t y) {
        const size_t i = (size_t(y) * f.width + x) * f.channels;
        return int(f.pixels[i]) + int(f.pixels[i + 1]) + int(f.pixels[i + 2]);
    };
    for (uint32_t y = 1; y + 1 < f.height; ++y) {
        for (uint32_t x = 1; x + 1 < f.width; ++x) {
            const int c = luma(x, y);
            int worst = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    worst = std::max(worst, std::abs(luma(x + dx, y + dy) - c));
            if (worst <= threshold * 3) mask[size_t(y) * f.width + x] = 1;
        }
    }
    return mask;
}

/// mean_abs_delta restricted to the masked pixels, and the fraction of the frame they cover.
static std::pair<double, double> masked_mean_abs_delta(const Frame& a, const Frame& b,
                                                       const std::vector<uint8_t>& mask) {
    if (!same_extent(a, b) || mask.size() != size_t(a.width) * a.height) return {-1.0, 0.0};
    double acc = 0.0;
    long long n = 0;
    for (size_t p = 0; p < mask.size(); ++p) {
        if (mask[p] == 0) continue;
        const size_t i = p * a.channels;
        acc += std::abs(int(a.pixels[i]) - int(b.pixels[i]));
        ++n;
    }
    return {n > 0 ? acc / double(n) : 0.0, double(n) / double(mask.size())};
}

/// SSR's per-frame parameters, applied to the LIVE pipeline config rather than to the AppConfig.
/// `ssr_quality` is a preset that rewrites several of them when the render config is built, so a
/// value set before construction never reaches the pipeline -- the probe then reads exactly like
/// the baseline, which is indistinguishable from "this parameter does not matter". Verified by
/// the [effective] line each test prints.
static void apply_live_ssr_overrides(toy::render::ToyRenderConfig& rc) {
    const char* raw = std::getenv("TOY_FLICKER_OFF");
    if (raw == nullptr) return;
    const std::string list(raw);
    auto tok = [&](const char* name) {
        for (size_t b = 0, e; b <= list.size(); b = e + 1) {
            e = list.find(',', b);
            if (e == std::string::npos) e = list.size();
            if (e > b && list.compare(b, e - b, name) == 0) return true;
        }
        return false;
    };
    if (tok("ssrjitter")) rc.ssr_jitter          = 0.0f;  // same ray every frame
    if (tok("ssrblur"))   rc.ssr_blur_radius     = 3.0f;  // wider spatial denoise
    if (tok("ssrfast"))   rc.ssr_temporal_frames = 8;     // converge in 8 frames, not 32
    if (tok("ssrslow"))   rc.ssr_temporal_frames = 128;   // converge far slower
    if (tok("ssrnotemporal")) rc.ssr_temporal_enabled = false;  // no accumulation at all
}

/// Box-downsamples a capture by an integer factor. The supersampling reference below needs the
/// two sequences at one size to be comparable at all, and a box filter over whole texels is the
/// honest reconstruction: it is what the 1x render would produce if its shading were band-limited.
static Frame box_downsample(const Frame& src, uint32_t factor) {
    Frame out;
    out.width    = src.width / factor;
    out.height   = src.height / factor;
    out.channels = src.channels;
    out.pixels.assign(size_t(out.width) * out.height * out.channels, 0);
    const uint32_t taps = factor * factor;
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            for (uint32_t c = 0; c < out.channels; ++c) {
                uint32_t sum = 0;
                for (uint32_t sy = 0; sy < factor; ++sy) {
                    for (uint32_t sx = 0; sx < factor; ++sx) {
                        const size_t si = ((size_t(y) * factor + sy) * src.width +
                                           (size_t(x) * factor + sx)) * src.channels + c;
                        sum += src.pixels[si];
                    }
                }
                out.pixels[(size_t(y) * out.width + x) * out.channels + c] =
                    uint8_t((sum + taps / 2) / taps);
            }
        }
    }
    return out;
}

/// Renders the shared trajectory and returns one capture per step, at `rw` x `rh`.
/// `shade` is applied to the live pipeline so a caller can measure one raw channel.
static std::vector<Frame> sweep_frames(uint32_t rw, uint32_t rh, const std::string& shade) {
    toy::core::AppConfig config = make_shipped_config(voxel_terrain_demo(), rw, rh);
    config.render.auto_exposure_enabled = false;
    // greenhollow's own scene settings ship aa_mode "off" -- the configuration the artifact was
    // reported under, and the default here. TOY_CRAWL_AA overrides it (taa, smaa, fxaa) for the
    // comparison that separates "this tile set shimmers" from "nothing is anti-aliasing this
    // scene": terrain_demo, the scene that looks fine, inherits taa from config.yaml, so the two
    // scenes never rendered with the same AA in the first place.
    // Setting aa_mode on the AppConfig is NOT enough: the scene's own `settings.render` block
    // (greenhollow ships `aa_mode: "off"` there) is applied when the scene loads and overrides
    // it -- measured: taa vs off differed by 426 pixels of 2M, i.e. TAA never engaged. So the
    // override is spliced into a VARIANT of the scene file instead, which is the one place the
    // scene cannot stomp.
    const char* aa_env = std::getenv("TOY_CRAWL_AA");
    config.render.aa_mode = (aa_env != nullptr && *aa_env != '\0') ? aa_env : "off";
    toy::core::Engine engine(std::move(config));
    if (!shade.empty()) engine.render_config().debug_view = shade;

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return {};
    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    if (terrain == nullptr || controller == nullptr) return {};

    shrink_terrain_and_centre(*terrain, *camera->scene);
    controller->follow_smoothing   = 0.0f;
    controller->movement_smoothing = 0.0f;
    controller->distance           = 18.0f;
    controller->pitch_deg          = 18.0f;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
    tick_frames(engine, 90);

    // A deliberately SLOW sweep: 0.12 deg/frame is a fraction of a pixel of screen motion at
    // this field of view, so a correctly filtered image may only change a little between
    // frames. Crawl is precisely the failure to do that -- an aliased surface re-snaps its
    // detail to the pixel grid on a sub-pixel move -- so a slow sweep separates the two where
    // a fast one buries both under honest parallax.
    std::vector<Frame> frames;
    for (int i = 0; i < 12; ++i) {
        controller->yaw_deg += 0.12f;
        engine.tick();
        frames.push_back(engine.capture_image(/*low_res=*/true));
    }
    return frames;
}

/**
 * @brief Under sub-pixel camera motion the image must change no faster than the true image does.
 *
 * The other half of "flickers while moving, settles when stopped". A temporal filter that has
 * not converged produces it (that is what moving_image_matches_its_converged_image measures),
 * but so does plain spatial aliasing: an under-filtered surface is a DETERMINISTIC function of
 * the camera pose, so it is perfectly still at rest and crawls the instant the pose moves, and
 * no temporal measurement can see it -- at a repeated pose it reproduces exactly.
 *
 * What separates them is a reference image that cannot alias. The same trajectory is rendered
 * twice, once at the test resolution and once at twice it, and the high-resolution sequence is
 * box-downsampled back. Both sequences then show the same content; the supersampled one just
 * resolves detail the other has to fold. If the 1x image changes much more per frame than the
 * supersampled one does, the excess is detail crawling across the pixel grid -- shimmer -- and
 * not anything the scene is actually doing.
 */
COOPA_TEST(sub_pixel_motion_does_not_crawl) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    const char* shade_env = std::getenv("TOY_CRAWL_VIEW");
    const std::string shade = (shade_env != nullptr) ? shade_env : "";

    const std::vector<Frame> native = sweep_frames(960, 540, shade);
    const std::vector<Frame> super  = sweep_frames(1920, 1080, shade);
    expect(native.size() == super.size() && !native.empty(), "crawl: both sweeps rendered");
    if (native.size() != super.size() || native.empty()) return;

    double native_step = 0.0, super_step = 0.0;
    for (size_t i = 1; i < native.size(); ++i) {
        native_step += mean_abs_delta(native[i], native[i - 1]);
        const Frame a = box_downsample(super[i], 2);
        const Frame b = box_downsample(super[i - 1], 2);
        super_step += mean_abs_delta(a, b);
    }
    native_step /= double(native.size() - 1);
    super_step  /= double(native.size() - 1);
    const double ratio = super_step > 1e-9 ? native_step / super_step : 0.0;

    std::cerr << "         [crawl] view=" << (shade.empty() ? "final" : shade)
              << " native_step=" << native_step << " supersampled_step=" << super_step
              << " ratio=" << ratio << "\n";

    expect(ratio < 1.35, "crawl: sub-pixel motion moves the image no faster than the true image (" +
                             std::to_string(ratio) + "x)");
}

/// The game this artifact was reported from, its own config and project root (so its assets and
/// its `assets/config.yaml` are what load). Overridable with TOY_GAME_DIR / TOY_GAME_SCENE.
static std::filesystem::path game_dir() {
    const char* env = std::getenv("TOY_GAME_DIR");
    return (env != nullptr && *env != '\0') ? std::filesystem::path(env)
                                            : std::filesystem::path("/home/coopa/git/cubeworld");
}

/// Renders the slow sub-pixel sweep on the game's own scene at `rw` x `rh`.
static std::vector<Frame> game_sweep(uint32_t rw, uint32_t rh, const std::string& shade) {
    const std::filesystem::path root = game_dir();
    const char* scene_env = std::getenv("TOY_GAME_SCENE");
    std::string scene = (scene_env != nullptr && *scene_env != '\0')
        ? scene_env : "assets/scenes/islands/greenhollow/scene.yaml";

    toy::core::AppConfig config = toy::core::AppConfig::load((root / "assets/config.yaml").string());
    config.window.width   = rw * 2;
    config.window.height  = rh * 2;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.render_width  = rw;
    config.render.render_height = rh;
    config.render.auto_exposure_enabled = false;
    // The game's HUD and its inventory panel cover a large, permanently static part of the
    // frame. They pass the flat-interior test and never change, so leaving them in halves the
    // measured per-frame delta of whatever the terrain is doing underneath.
    config.render.screen_ui_enabled = false;
    config.render.world_ui_enabled  = false;
    config.scene.default_scene  = scene;
    config.output.save_on_exit  = false;

    toy::core::EngineOptions options;
    options.project_root = root;
    options.boot_splash  = false;
    options.set_app_icon = false;
    toy::core::Engine engine(std::move(config), options);
    if (!shade.empty()) engine.render_config().debug_view = shade;
    // TOY_CRAWL_LODBIAS pins the mesh LOD selection (RUNTIME key): a large bias keeps every
    // chunk at LOD 0, so any per-frame change that disappears under it was a chunk
    // re-triangulating as its screen size crossed a level threshold -- a real geometry change
    // that survives TAA, every render-feature toggle, and every still-image comparison.
    if (const char* lb = std::getenv("TOY_CRAWL_LODBIAS"); lb != nullptr && *lb != '\0') {
        engine.render_config().mesh_lod_bias = float(std::atof(lb));
    }

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return {};
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    if (controller == nullptr) return {};
    controller->follow_smoothing   = 0.0f;
    controller->movement_smoothing = 0.0f;

    // The world streams terrain around the camera and this one is far larger than the test
    // scene's, so give it a real budget before anything is measured.
    // `TOY_CRAWL_MOVE=run` drives the orbit target across the world at a player's running
    // speed instead of swinging the camera around a fixed point. Rotation at a fixed radius
    // changes almost no distances, so it exercises neither terrain streaming, nor mesh LOD, nor
    // a shadow cascade re-fitting around a moving focus point -- all of which a player running
    // across the island does continuously, and all of which stop the moment they stand still.
    const char* move_env = std::getenv("TOY_CRAWL_MOVE");
    const std::string move = (move_env != nullptr) ? move_env : "";
    const bool  run  = move == "run";
    // `hold` keeps the camera completely still. With a live clock that isolates what the WORLD
    // does per frame -- water, foliage, the sun, the streamer -- from what camera motion adds,
    // which is the difference between "my scene animates" and "my renderer flickers when I move".
    const bool  hold = move == "hold";
    const float run_step = 0.12f;           // world units per frame; ~7 u/s at 60 Hz
    // Move the object the orbit FOLLOWS, not the controller's static `target`: this scene tracks
    // the player, so `target` is an unused (0,0,0) and writing it teleports the camera off the
    // island to the world origin, where the measurement is of empty sky. Moving the tracked
    // object is also what a player does.
    coopa::scene::SceneObject* runner =
        run ? camera->scene->find_object(controller->tracker) : nullptr;
    if (run && runner == nullptr) {
        std::cerr << "         [sweep] no tracker object '" << controller->tracker
                  << "' to drive; refusing to measure a camera that will not move\n";
        return {};
    }
    const glm::vec3 start_pos =
        runner != nullptr ? runner->get_transform()->transform().position() : glm::vec3(0.0f);

    auto advance = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            if (runner != nullptr) {
                auto& t = runner->get_transform()->transform();
                t.set_position(t.position() + glm::vec3(run_step, 0.0f, 0.0f));
            } else if (!hold) {
                controller->yaw_deg += 0.12f;
            }
            engine.tick();
        }
    };

    if (auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>())
        tick_until(engine, 1800, [&] { return count_live_chunks(*terrain) >= 20; });
    tick_frames(engine, 150);

    // Walk the path once so the terrain along it is already streamed, then start over: a chunk
    // arriving mid-measurement is a real difference that belongs to the streamer, not to
    // anything this metric is about.
    if (run) {
        advance(14);
        runner->get_transform()->transform().set_position(start_pos);
        tick_frames(engine, 90);
    }

    std::vector<Frame> frames;
    // How far the CAMERA itself travels per frame, printed with the result. Without it a bigger
    // per-frame image delta cannot be told apart from a camera that simply moved further --
    // under a live clock the tracked object is a physics body and can be falling.
    double cam_travel = 0.0;
    glm::vec3 prev_cam = camera->owner->get_transform()->transform().position();
    for (int i = 0; i < 12; ++i) {
        advance(1);
        const glm::vec3 now = camera->owner->get_transform()->transform().position();
        cam_travel += double(glm::length(now - prev_cam));
        prev_cam = now;
        frames.push_back(engine.capture_image(/*low_res=*/true));
    }
    std::cerr << "         [sweep] " << rw << "x" << rh << " camera travel/frame="
              << (cam_travel / 12.0) << " wu\n";
    // A consecutive PAIR at full resolution, for looking at rather than measuring. Every metric
    // in this file reduces a frame to one number, and a number cannot show the SHAPE of an
    // artifact -- which is what says whether it is texture crawl, a geometry seam, or a tile
    // grid. Only from the larger sweep, so the two sizes do not overwrite each other.
    if (std::getenv("TOY_CRAWL_DUMP") != nullptr && rw >= 1920) {
        dump_frame(frames[6], "pair_a");
        dump_frame(frames[7], "pair_b");
    }
    return frames;
}

/**
 * @brief The same crawl contract as sub_pixel_motion_does_not_crawl, on the GAME's own scene.
 *
 * Needed because the measurement is only as good as the content it runs on. terrain_demo is a
 * ~200-unit world of bare blocks; the reported artifact is in a 2048-unit island with water,
 * snow and foliage, whose scene file turns AA off in its own settings. Both the world scale and
 * the surface mix are things an aliasing or precision artifact is sensitive to, so a clean
 * number on the small scene proves nothing about the big one.
 */
COOPA_TEST(game_scene_sub_pixel_motion_does_not_crawl) {
    // The supersampled reference makes a LIVE clock safe here, unlike the converged-image
    // metrics: both sweeps step the same FIXED_DT over the same seeded world, so the scene's own
    // animation appears identically in both and cancels out of the ratio. TOY_CRAWL_DT=0.0166
    // therefore measures aliasing with water, foliage, the sun and the streamer all running.
    const char* dt_env = std::getenv("TOY_CRAWL_DT");
    ScopedEnv fixed_dt("FIXED_DT", (dt_env != nullptr && *dt_env != '\0') ? dt_env : "0");
    ScopedEnv no_input("NO_INPUT", "1");

    if (!std::filesystem::exists(game_dir() / "assets/config.yaml")) {
        std::cerr << "         [crawl] no game at " << game_dir() << ", skipping\n";
        return;
    }
    const char* shade_env = std::getenv("TOY_CRAWL_VIEW");
    const std::string shade = (shade_env != nullptr) ? shade_env : "";

    const std::vector<Frame> native = game_sweep(960, 540, shade);
    const std::vector<Frame> super  = game_sweep(1920, 1080, shade);
    expect(native.size() == super.size() && !native.empty(), "game crawl: both sweeps rendered");
    if (native.size() != super.size() || native.empty()) return;

    double native_step = 0.0, super_step = 0.0, flat_native = 0.0, flat_super = 0.0, cover = 0.0;
    for (size_t i = 1; i < native.size(); ++i) {
        const Frame sa = box_downsample(super[i], 2);
        const Frame sb = box_downsample(super[i - 1], 2);
        native_step += mean_abs_delta(native[i], native[i - 1]);
        super_step  += mean_abs_delta(sa, sb);
        const std::vector<uint8_t> mask = flat_interior_mask(sb);
        const auto fn = masked_mean_abs_delta(native[i], native[i - 1], mask);
        const auto fs = masked_mean_abs_delta(sa, sb, mask);
        flat_native += fn.first;
        flat_super  += fs.first;
        cover       += fn.second;
    }
    const double legs = double(native.size() - 1);
    native_step /= legs; super_step /= legs; flat_native /= legs; flat_super /= legs; cover /= legs;
    const double ratio      = super_step > 1e-9 ? native_step / super_step : 0.0;
    const double flat_ratio = flat_super > 1e-9 ? flat_native / flat_super : 0.0;

    std::cerr << "         [game crawl] view=" << (shade.empty() ? "final" : shade)
              << " native_step=" << native_step << " supersampled_step=" << super_step
              << " ratio=" << ratio << " | flat interiors (" << (cover * 100.0) << "% of frame)"
              << " native=" << flat_native << " supersampled=" << flat_super
              << " ratio=" << flat_ratio << "\n";

    expect(ratio < 1.35, "game crawl: sub-pixel motion moves the image no faster than the true "
                         "image (" + std::to_string(ratio) + "x)");
}

/// Runs the slow sweep on the game's scene with the editor's own presentation setup:
/// `resolution_mode: fill` at `render_height`, displayed into a `win_w` x `win_h` region, and
/// captured at DISPLAY resolution (after the render -> display resample) rather than at render
/// resolution. render_height == win_h makes that resample 1:1 and is the control.
static std::vector<Frame> game_display_sweep(uint32_t win_w, uint32_t win_h, uint32_t render_height) {
    const std::filesystem::path root = game_dir();
    const char* scene_env = std::getenv("TOY_GAME_SCENE");
    const std::string scene = (scene_env != nullptr && *scene_env != '\0')
        ? scene_env : "assets/scenes/islands/greenhollow/scene.yaml";

    toy::core::AppConfig config = toy::core::AppConfig::load((root / "assets/config.yaml").string());
    config.window.width   = win_w;
    config.window.height  = win_h;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.resolution_mode = "fill";       // apply_editor_render_overrides()
    config.render.upscale_mode    = "fit";
    config.render.render_height   = render_height;
    // Eye adaptation re-meters the whole frame every frame and eases exposure toward it, so
    // camera motion drives a global brightness glide that settles once the camera does. Every
    // measurement in this file excludes it to see past it; TOY_CRAWL_EXPOSURE puts the shipped
    // behaviour back, because "the surfaces shimmer while I move" is also what that glide looks
    // like on a frame full of large flat faces.
    config.render.auto_exposure_enabled = std::getenv("TOY_CRAWL_EXPOSURE") != nullptr;
    // The game's HUD and its inventory panel cover a large, permanently static part of the
    // frame. They pass the flat-interior test and never change, so leaving them in halves the
    // measured per-frame delta of whatever the terrain is doing underneath.
    config.render.screen_ui_enabled = false;
    config.render.world_ui_enabled  = false;
    apply_flicker_overrides(config.render);
    config.scene.default_scene  = scene;
    config.output.save_on_exit  = false;

    toy::core::EngineOptions options;
    options.project_root = root;
    options.boot_splash  = false;
    options.set_app_icon = false;
    toy::core::Engine engine(std::move(config), options);

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return {};
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    if (controller == nullptr) return {};
    controller->follow_smoothing   = 0.0f;
    controller->movement_smoothing = 0.0f;
    // The recordings are taken from the game's own orbit distance looking down at the ground a
    // few metres away, not from the wide establishing pose the scene happens to load at -- and
    // ground that fills the frame is the whole subject of the measurement.
    controller->distance  = 24.0f;
    controller->pitch_deg = 50.0f;

    if (auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>())
        tick_until(engine, 1800, [&] { return count_live_chunks(*terrain) >= 20; });
    tick_frames(engine, 150);

    const char* speed_env = std::getenv("TOY_CRAWL_SPEED");
    const float speed = (speed_env != nullptr) ? float(std::atof(speed_env)) : 0.12f;
    std::vector<Frame> frames;
    for (int i = 0; i < 12; ++i) {
        controller->yaw_deg += speed;
        engine.tick();
        frames.push_back(engine.capture_image(/*low_res=*/false));
    }
    std::cerr << "         [display sweep] render=" << engine.pipeline().render_width() << "x"
              << engine.pipeline().render_height() << " capture=" << frames[0].width << "x"
              << frames[0].height << " live_chunks="
              << (camera->scene->find_first_component<toy::world::TerrainComponent>()
                      ? count_live_chunks(*camera->scene->find_first_component<toy::world::TerrainComponent>()) : -1)
              << "\n";
    if (std::getenv("TOY_CRAWL_DUMP") != nullptr)
        dump_frame(frames[0], "display_sweep_" + std::to_string(render_height));
    return frames;
}

/**
 * @brief Presenting the render target into a SMALLER display region must not make the image
 *        crawl under camera motion.
 *
 * Where the reported flicker lives. Every measurement above captures the render target, which
 * is clean -- but that is not what anyone looks at. The composite resamples the render target
 * into the display region with a NEAREST sampler, which is right for the pixel-art upscale it
 * was written for (window larger than the render target) and is point-sampled MINIFICATION the
 * moment the display region is smaller. The editor makes it smaller as a matter of course:
 * `apply_editor_render_overrides()` switches the project to `resolution_mode: fill`, which
 * keeps the project's `render_height` -- 1080 -- while the viewport panel inside the editor
 * window is a few hundred pixels shorter.
 *
 * Point-sampled minification keeps one source texel per destination pixel and discards the
 * rest, so which detail survives is re-chosen every time the camera moves a fraction of a
 * pixel, and stops being re-chosen the moment it holds still. That is the complaint exactly:
 * flat, finely detailed faces sparkle while moving and settle when stopped, with AA and AO
 * making no difference -- neither is in this path.
 *
 * The control renders at the display height, so the same frame reaches the screen through a
 * 1:1 copy. Any excess the downscaled run shows over it belongs to the resample.
 */
COOPA_TEST(display_downscale_does_not_crawl) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    if (!std::filesystem::exists(game_dir() / "assets/config.yaml")) {
        std::cerr << "         [downscale] no game at " << game_dir() << ", skipping\n";
        return;
    }

    // 998 x 730 is the editor window in the recordings this came from; 1080 is the project's
    // render_height, which fill mode keeps.
    const std::vector<Frame> downscaled = game_display_sweep(998, 730, 1080);
    const std::vector<Frame> one_to_one = game_display_sweep(998, 730, 730);
    expect(downscaled.size() == one_to_one.size() && !downscaled.empty(),
           "downscale: both sweeps rendered");
    if (downscaled.size() != one_to_one.size() || downscaled.empty()) return;

    double down_flat = 0.0, flat_1to1 = 0.0, cover = 0.0;
    for (size_t i = 1; i < downscaled.size(); ++i) {
        const std::vector<uint8_t> mask = flat_interior_mask(one_to_one[i - 1]);
        const auto d = masked_mean_abs_delta(downscaled[i], downscaled[i - 1], mask);
        const auto o = masked_mean_abs_delta(one_to_one[i], one_to_one[i - 1], mask);
        down_flat += d.first;
        flat_1to1 += o.first;
        cover     += o.second;
    }
    const double legs = double(downscaled.size() - 1);
    down_flat /= legs; flat_1to1 /= legs; cover /= legs;
    const double ratio = flat_1to1 > 1e-9 ? down_flat / flat_1to1 : 0.0;

    std::cerr << "         [downscale] flat interiors (" << (cover * 100.0) << "% of frame)"
              << " render1080->display730=" << down_flat
              << " render730->display730=" << flat_1to1 << " ratio=" << ratio << "\n";

    expect(ratio < 1.5, "downscale: presenting into a smaller region does not add crawl (" +
                            std::to_string(ratio) + "x)");
}

/**
 * @brief The converged-image contract, on the game's own scene with the camera TRANSLATING.
 *
 * moving_image_matches_its_converged_image swings the camera around a fixed point on a small
 * test world. Orbiting at a fixed radius barely changes any distance, so it exercises almost no
 * disocclusion, no terrain streaming, no mesh LOD switch and no shadow cascade re-fit -- the
 * things a player running across the island drives continuously, and the things that stop the
 * instant they stand still. This is that case: the followed object is walked across the world,
 * on the content the artifact was reported from.
 *
 * FIXED_DT=0 throughout, so the still-camera control is a true zero and the headline number
 * needs no subtraction -- see this file's other tests for what a live clock does to it.
 */
COOPA_TEST(game_running_image_matches_its_converged_image) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    if (!std::filesystem::exists(game_dir() / "assets/config.yaml")) {
        std::cerr << "         [game motion] no game at " << game_dir() << ", skipping\n";
        return;
    }

    const std::filesystem::path root = game_dir();
    const char* scene_env = std::getenv("TOY_GAME_SCENE");
    const std::string scene = (scene_env != nullptr && *scene_env != '\0')
        ? scene_env : "assets/scenes/islands/greenhollow/scene.yaml";

    toy::core::AppConfig config = toy::core::AppConfig::load((root / "assets/config.yaml").string());
    config.window.width   = 1920;
    config.window.height  = 1080;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.render_width  = 1920;
    config.render.render_height = 1080;
    config.render.auto_exposure_enabled = false;
    config.render.screen_ui_enabled = false;
    config.render.world_ui_enabled  = false;
    const std::string disabled = apply_flicker_overrides(config.render);
    config.scene.default_scene  = scene;
    config.output.save_on_exit  = false;

    toy::core::EngineOptions options;
    options.project_root = root;
    options.boot_splash  = false;
    options.set_app_icon = false;
    toy::core::Engine engine(std::move(config), options);

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
           "game motion: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    expect(controller != nullptr, "game motion: the camera has a controller");
    if (controller == nullptr) return;
    controller->follow_smoothing   = 0.0f;   // stop dead, so the tail measured is the renderer's
    controller->movement_smoothing = 0.0f;
    controller->distance  = 24.0f;
    controller->pitch_deg = 50.0f;

    apply_live_ssr_overrides(engine.render_config());

    // What the pipeline ACTUALLY ended up with. `ssr_quality` is a preset that writes several
    // of these, so a value set on the AppConfig can be silently overwritten before the pipeline
    // sees it -- which makes a parameter probe come back reading exactly like the baseline, for
    // the one reason that looks identical to "this parameter does not matter".
    {
        const auto& rc = engine.pipeline().render_config();
        std::cerr << "         [effective] ssr=" << rc.ssr_enabled << " jitter=" << rc.ssr_jitter
                  << " blur_radius=" << rc.ssr_blur_radius
                  << " temporal_frames=" << rc.ssr_temporal_frames
                  << " half_res=" << rc.ssr_half_res
                  << " temporal_enabled=" << rc.ssr_temporal_enabled << "\n";
    }

    coopa::scene::SceneObject* runner = camera->scene->find_object(controller->tracker);
    expect(runner != nullptr && runner->get_transform() != nullptr,
           "game motion: the orbit's tracked object exists to drive");
    if (runner == nullptr || runner->get_transform() == nullptr) return;

    if (auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>())
        tick_until(engine, 1800, [&] { return count_live_chunks(*terrain) >= 20; });
    tick_frames(engine, 150);

    auto run_frames = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            auto& t = runner->get_transform()->transform();
            t.set_position(t.position() + glm::vec3(0.12f, 0.0f, 0.0f));
            engine.tick();
        }
    };

    std::vector<double> gaps, still;
    for (int leg = 0; leg < 4; ++leg) {
        run_frames(10);
        const Frame moving = engine.capture_image(/*low_res=*/true);
        tick_frames(engine, 60);                       // stand still and let everything converge
        const Frame converged = engine.capture_image(true);
        gaps.push_back(mean_abs_delta(moving, converged));
        tick_frames(engine, 60);                       // control: still standing still
        still.push_back(mean_abs_delta(converged, engine.capture_image(true)));
    }

    double sum = 0.0, still_sum = 0.0;
    for (double g : gaps)  sum += g;
    for (double g : still) still_sum += g;
    const double mean       = sum / double(gaps.size());
    const double still_mean = still_sum / double(still.size());

    std::cerr << "         [game motion] disabled=" << disabled << " motion=" << mean
              << " still=" << still_mean << " excess=" << (mean - still_mean) << " legs:";
    for (double g : gaps) std::cerr << " " << g;
    std::cerr << "\n";

    expect(mean - still_mean < 0.60,
           "game motion: a running camera's image tracks its converged image (" +
               std::to_string(mean - still_mean) + " levels/px)");
}

/**
 * @brief No pixel of the game's scene is decided by a coplanar depth TIE.
 *
 * The hypothesis every other test in this file is structurally blind to: two faces occupying
 * the same plane, with the depth test picking between them per pixel. Such a pixel is a
 * deterministic function of the camera pose, so it is perfectly still at rest and re-decides
 * itself the moment the pose moves -- "flickers while moving, settles when stopped" -- and it
 * is decided in the depth test, so turning every render feature off changes nothing about it.
 * A metric that compares two frames at the SAME pose (moving vs converged) cancels it exactly,
 * and a supersampled reference reproduces it identically, which is why both read clean.
 *
 * The probe is exact rather than statistical. `VK_COMPARE_OP_LESS` and `_LESS_OR_EQUAL` give
 * different answers for one input only: fragments at EQUAL depth, where LESS keeps the first
 * drawn and LEQUAL the last. Everything else in the frame -- every ordering, every shaded
 * value -- is bit-identical between the two. So the pixels that change ARE the coplanar ties,
 * with no threshold to tune and nothing else able to contaminate the count.
 *
 * Rendered as raw G-buffer albedo (debug_view), so a tie shows up as the two surfaces' own
 * colours rather than as a lighting difference, and so no temporal filter is in the path.
 */
COOPA_TEST(no_coplanar_depth_ties) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    if (!std::filesystem::exists(game_dir() / "assets/config.yaml")) {
        std::cerr << "         [ties] no game at " << game_dir() << ", skipping\n";
        return;
    }
    const std::filesystem::path root = game_dir();
    const char* scene_env = std::getenv("TOY_GAME_SCENE");
    const std::string scene = (scene_env != nullptr && *scene_env != '\0')
        ? scene_env : "assets/scenes/islands/greenhollow/scene.yaml";

    // ONE engine, both captures. The probe's whole claim is that the two frames differ in
    // nothing but the depth compare -- and two separate Engines over a live island cannot give
    // that: they stream their chunks on wall-clock time and tick a different number of frames,
    // so each capture holds its own world. Measured that way, the same build alternated between
    // 0 and ~4-6% of the frame "tied" from run to run, which is what sent a whole investigation
    // after the terrain mesher. Rebuilding the pipeline in place with TOY_DEPTH_LEQUAL set flips
    // exactly one thing (gbuffer_pipeline.h reads it while building) and leaves the scene, the
    // streamed chunks and the frame index where they were.
    toy::core::AppConfig config = toy::core::AppConfig::load((root / "assets/config.yaml").string());
    config.window.width   = 1920;
    config.window.height  = 1080;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.render_width  = 1920;
    config.render.render_height = 1080;
    config.render.auto_exposure_enabled = false;
    config.render.screen_ui_enabled = false;
    config.render.world_ui_enabled  = false;
    config.scene.default_scene  = scene;
    config.output.save_on_exit  = false;

    toy::core::EngineOptions options;
    options.project_root = root;
    options.boot_splash  = false;
    options.set_app_icon = false;

    toy::core::Engine engine(std::move(config), options);
    engine.render_config().debug_view = "albedo";

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
           "ties: the game scene has a camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
        controller->distance  = 24.0f;
        controller->pitch_deg = 50.0f;
    }
    if (auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>()) {
        // Settled, not merely started: a chunk still arriving would land between the two
        // captures and read as a tie.
        int stable = 0, last_count = -1;
        tick_until(engine, 4000, [&] {
            const int n = count_live_chunks(*terrain);
            stable = (n == last_count) ? stable + 1 : 0;
            last_count = n;
            return stable >= 60 && terrain->is_ready();
        });
    }
    tick_frames(engine, 150);

    const Frame first = engine.capture_image(/*low_res=*/true);
    {
        // By value: rebuild_pipeline() destroys the old pipeline before it builds the new one,
        // so a reference into its config would dangle halfway through.
        const toy::render::ToyRenderConfig cfg = engine.pipeline().render_config();
        ScopedEnv leq("TOY_DEPTH_LEQUAL", "1");
        engine.rebuild_pipeline(cfg);
    }
    engine.render_config().debug_view = "albedo";
    // The rebuild restarts every temporal filter, and the debug view still travels the post
    // chain -- so the second capture needs the same settle the first one had, or what gets
    // counted is TAA converging rather than a depth tie (measured: 2-6% of the frame, varying
    // run to run, with only 3 frames here).
    tick_frames(engine, 150);
    const Frame last = engine.capture_image(/*low_res=*/true);

    expect(first.width > 0 && same_extent(first, last), "ties: both captures rendered");
    if (first.width == 0 || !same_extent(first, last)) return;

    const long long tied = count_diff(first, last, /*tolerance=*/2);
    const double pct = 100.0 * double(tied) / double(first.width * first.height);
    std::cerr << "         [ties] pixels decided by a coplanar depth tie: " << tied
              << " (" << pct << "% of the frame)\n";
    if (tied > 0) {
        dump_frame(first, "depth_tie_less");
        dump_frame(last,  "depth_tie_lequal");
    }

    // Reported, NOT asserted, and the reason is the probe itself. The claim it was built on --
    // "LESS and LESS_OR_EQUAL differ for coplanar fragments and for nothing else" -- holds for
    // the draw, but not for the measurement: the compare is read while the G-buffer pipeline is
    // BUILT, so the second frame needs a rebuilt pipeline, and a rebuild restarts every temporal
    // filter and the jitter phase (the debug view still travels the post chain). Measured on a
    // static engine scene, one engine, settled world and 150 frames of settle on both sides, the
    // same build reported 9.16% and then 0.17% of the frame; on greenhollow, 2.74% twice and
    // 5.87% once. The number is noise at the scale the contract needed (0.05%), and a red test
    // that encodes an unmeasurable claim is worse than an honest reading: this once sent a whole
    // investigation into the terrain mesher's merge T-junctions, which turned out to be
    // blameless (TOY_TERRAIN_NO_MERGE "fixing" it was the same coin-flip).
    //
    // To make it a contract again, the two frames have to come from ONE pipeline -- e.g. a
    // runtime depth-compare switch drawing both variants in a frame -- not from a rebuild.
    std::cerr << "         [ties] (reported only -- see the comment: this probe cannot isolate "
                 "the depth compare below its own noise)\n";
}

/**
 * @brief After the camera stops, no pixel may keep flapping between two values.
 *
 * The signature from Coopa's CAPTURE_RING frames (cubeworld, 2026-10-18): during the settle,
 * pixels on camera-facing walls flip between exactly two discrete values at constant amplitude
 * (e.g. 120 <-> 127) for dozens of frames, then lock solid once the freeze deadband engages.
 * Constant-amplitude binary flipping is not a converging average -- a running average's
 * per-frame change shrinks as 1/n -- it is either raw per-frame stochastic output (history
 * rejected, count pinned at 1) or two ping-pong parities converged to different fixed points.
 * Either way it reads as the reported "textures flicker, then lag, then settle".
 *
 * Metric: hold the camera after a run, capture consecutive frames, and count pixels whose
 * value toggles by more than `kFlipLevels` AND returns to an earlier value -- a flip, not a
 * drift. TOY_FLICKER_OFF attributes it per pass.
 */
COOPA_TEST(no_binary_flipping_after_camera_stops) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");   // real time: freeze deadbands and smoothing run
    ScopedEnv no_input("NO_INPUT", "1");

    if (!std::filesystem::exists(game_dir() / "assets/config.yaml")) {
        std::cerr << "         [flip] no game at " << game_dir() << ", skipping\n";
        return;
    }
    const std::filesystem::path root = game_dir();
    toy::core::AppConfig config = toy::core::AppConfig::load((root / "assets/config.yaml").string());
    config.window.width   = 1920;
    config.window.height  = 1080;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.render_width  = 1920;
    config.render.render_height = 1080;
    config.render.auto_exposure_enabled = false;
    config.render.screen_ui_enabled = false;
    config.render.world_ui_enabled  = false;
    const std::string disabled = apply_flicker_overrides(config.render);
    config.scene.default_scene  = "assets/scenes/islands/greenhollow/scene.yaml";
    config.output.save_on_exit  = false;

    toy::core::EngineOptions options;
    options.project_root = root;
    options.boot_splash  = false;
    options.set_app_icon = false;
    toy::core::Engine engine(std::move(config), options);
    apply_live_ssr_overrides(engine.render_config());
    // TOY_FLICKER_VIEW renders one raw channel instead of the lit image (runtime key). The
    // bisection that matters: flips already present in `albedo` live in the SURFACE stage
    // (the material/texture hypothesis); flips only in the final image live in lighting/post.
    if (const char* view = std::getenv("TOY_FLICKER_VIEW"); view != nullptr && *view != '\0') {
        engine.render_config().debug_view = view;
    }

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
           "flip: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    coopa::scene::SceneObject* runner =
        controller != nullptr ? camera->scene->find_object(controller->tracker) : nullptr;
    expect(controller != nullptr && runner != nullptr && runner->get_transform() != nullptr,
           "flip: the orbit and its tracked object exist");
    if (controller == nullptr || runner == nullptr || runner->get_transform() == nullptr) return;
    // The shipped smoothing stays: the settle lag it causes is part of what is being measured.
    controller->distance  = 24.0f;
    controller->pitch_deg = 50.0f;

    if (auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>())
        tick_until(engine, 1800, [&] { return count_live_chunks(*terrain) >= 20; });
    tick_frames(engine, 150);

    for (int i = 0; i < 20; ++i) {   // run...
        auto& t = runner->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(0.12f, 0.0f, 0.0f));
        engine.tick();
    }
    // ...and stop. Capture the settle, frame by frame.
    std::vector<Frame> hold;
    for (int i = 0; i < 24; ++i) {
        engine.tick();
        hold.push_back(engine.capture_image(/*low_res=*/true));
    }

    // A flip: the red channel moves by > kFlipLevels in one frame AND later returns to within
    // 1 level of a previous value. Water and other animation drifts; a two-state pixel flips.
    const int kFlipLevels = 4;
    const Frame& f0 = hold.front();
    long long flip_pixels = 0;
    for (uint32_t p = 0; p < f0.width * f0.height; ++p) {
        const std::size_t idx = std::size_t(p) * f0.channels;
        int lo = 256, hi = -1, jumps = 0;
        int prev = f0.pixels[idx];
        for (const Frame& f : hold) {
            const int v = f.pixels[idx];
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            if (std::abs(v - prev) > kFlipLevels) ++jumps;
            prev = v;
        }
        // Two-state: at least 3 jumps of amplitude, but total range barely above one jump --
        // i.e. it keeps returning. A drifting pixel accumulates range; a flapping one does not.
        if (jumps >= 3 && (hi - lo) <= 2 * kFlipLevels + 2) ++flip_pixels;
    }
    const double pct = 100.0 * double(flip_pixels) / double(f0.width * f0.height);
    std::cerr << "         [flip] disabled=" << disabled << " flip_pixels=" << flip_pixels
              << " (" << pct << "% of frame)\n";
    expect(pct < 0.02, "flip: no pixel keeps flapping between two values after the camera stops (" +
                           std::to_string(pct) + "%)");
}
