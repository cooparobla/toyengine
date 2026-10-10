/**
 * @file sky_render_test.cpp
 * @brief The skies and clouds on the GPU: the physical sky follows the clock (blue noon, red
 *        sunset, stars only at night), clouds follow coverage and dim the sun, CPU and GPU zenith
 *        agree; the gradient sky costs nothing, takes clouds too and round-trips exactly; low
 *        volumetric clouds cover the ground under a topdown camera, fade by height and cast
 *        shadows; the flat toon clouds draw from above and below and cast shadows. Cloud temporal
 *        stability is extended/temporal_stability.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/weather/weather_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("sky_render");

using namespace toy::test;

/**
 * @brief sky_test end to end: the physical sky renders, follows the clock (blue noon, red sunset,
 *        stars only at night), the clouds follow the coverage and dim the sun, the CPU's gradient
 *        colours match the GPU's sky, and the gradient sky is untouched by any of it.
 */
COOPA_TEST(physical_sky_follows_the_clock_and_coverage) {
    // FIXED_DT=0: nothing animates (water, the mannequins' idle, cloud drift), so A/B captures
    // differ only by what each step changes. The weather's state is set directly.
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/tests/rendering/sky_test/scene.yaml", 480, 270, 480, 270);
    config.render.aa_mode = "off";
    config.render.auto_exposure_enabled = false;   // A/B colours through one fixed exposure
    config.render.bloom_enabled = false;
    config.render.outline_enabled = false;
    // The shadow filter's per-frame rotation would make captures at different frames differ.
    config.render.shadows_enabled = false;
    toy::core::Engine engine(std::move(config));
    toy::weather::WeatherSystem* w = engine.weather();
    expect(w && w->enabled(), "sky_test: the scene's weather is on");
    if (!w) return;
    auto& pl = engine.pipeline();
    auto& cfg = pl.render_config_mut();
    // The scene's render overrides re-apply config.yaml's runtime keys over the test config, so
    // these are set live (a scene-settings change below re-applies them too -- see there).
    cfg.shadows_enabled = false;
    cfg.outline_enabled = false;

    // Noon, clear, clouds on: renders clean, and the weather drives the coverage.
    w->set_time(12.0f);
    w->set_condition("clear", 0.0f);
    tick_frames(engine, 4);
    expect(pl.physical_sky_active(), "sky_test: the physical sky is on (scene render sky_model: physical)");
    expect(pl.render_config().clouds, "sky_test: ...with clouds");
    expect_near(pl.render_config().cloud_coverage, w->state().cloud_cover, 1e-5f, "sky_test: the weather drives cloud_coverage");
    const Frame noon_clouds = engine.capture_image(true);
    expect(black_block_pixels(noon_clouds) == 0, "sky_test: noon with clouds has no black (NaN) blocks");
    expect(pl.sky_atmosphere_pass().lut_renders() == 1, "sky_test: the atmosphere tables render once");

    // The sky's own colour, clouds off: blue at noon, red at sunset (the camera faces west).
    cfg.clouds = false;
    tick_frames(engine, 2);
    const Frame noon = engine.capture_image(true);
    const glm::vec3 noon_h = band_mean(noon, 0.3f, 0.5f);
    if (!(noon_h.b > noon_h.r + 10.0f)) dump_frame(noon, "sky_test_noon");
    expect(noon_h.b > noon_h.r + 10.0f, "sky_test: the noon horizon is blue (r " + std::to_string(noon_h.r) + ", b " + std::to_string(noon_h.b) + ")");
    w->set_time(17.85f);
    tick_frames(engine, 2);
    const Frame dusk = engine.capture_image(true);
    const glm::vec3 dusk_h = band_mean(dusk, 0.45f, 0.53f);
    if (!(dusk_h.r > dusk_h.b + 10.0f)) dump_frame(dusk, "sky_test_dusk");
    expect(dusk_h.r > dusk_h.b + 10.0f, "sky_test: the sunset horizon is red (r " + std::to_string(dusk_h.r) + ", b " + std::to_string(dusk_h.b) + ")");
    const glm::vec3 dusk_tint = pl.sky_state().light_tint;
    expect(dusk_tint.r > dusk_tint.b * 2.0f, "sky_test: the setting sun's light is reddened by the air");
    expect(pl.sky_atmosphere_pass().lut_renders() == 1, "sky_test: the tables do not re-render when only the sun moves");

    // Stars: only at night.
    w->set_time(0.5f);
    tick_frames(engine, 2);
    const Frame night_stars = engine.capture_image(true);
    expect(band_mean(night_stars, 0.0f, 0.4f).b > 0.5f, "sky_test: the night sky is dark, not black");
    cfg.sky_stars = false;
    tick_frames(engine, 2);
    const Frame night_plain = engine.capture_image(true);
    const long long star_px = count_diff(night_stars, night_plain, 1);
    expect(star_px > 30, "sky_test: stars show at night (" + std::to_string(star_px) + " px)");
    // Enough frames for SSR's previous-frame colour (what reflections sample) to settle.
    w->set_time(12.0f);
    tick_frames(engine, 10);
    const Frame day_plain = engine.capture_image(true);
    cfg.sky_stars = true;
    tick_frames(engine, 10);
    const Frame day_stars = engine.capture_image(true);
    const long long day_star_px = count_diff(day_plain, day_stars, 0);
    if (day_star_px) { dump_frame(day_plain, "sky_day_plain"); dump_frame(day_stars, "sky_day_stars"); }
    expect(day_star_px == 0, "sky_test: ...and none by day (" + std::to_string(day_star_px) + " px)");

    // Coverage without the weather: 0 vs 1 changes the sky and dims the sun.
    fkyaml::node settings = fkyaml::node::mapping();
    settings["weather"] = fkyaml::node::deserialize(std::string("enabled: false\n"));
    engine.set_scene_settings(engine.scene(), settings);
    tick_frames(engine, 2);
    cfg.shadows_enabled = false;
    cfg.outline_enabled = false;
    cfg.sky_model = "physical";
    cfg.clouds = true;
    cfg.cloud_shadows = false;   // without shadows, the cover dims the sun uniformly
    cfg.cloud_coverage = 0.0f;
    tick_frames(engine, 2);
    const Frame clear = engine.capture_image(true);
    const glm::vec3 clear_tint = pl.sky_state().light_tint;
    cfg.cloud_coverage = 1.0f;
    tick_frames(engine, 2);
    const Frame overcast = engine.capture_image(true);
    const long long cover_px = count_diff(clear, overcast, 8);
    if (cover_px <= long(clear.width * clear.height) / 5) { dump_frame(clear, "sky_test_clear"); dump_frame(overcast, "sky_test_overcast"); }
    expect(cover_px > long(clear.width * clear.height) / 5, "sky_test: full coverage changes the sky (" + std::to_string(cover_px) + " px)");
    expect(pl.sky_state().light_tint.g < clear_tint.g * 0.5f, "sky_test: ...and dims the sun");
    cfg.cloud_shadows = true;
    tick_frames(engine, 2);
    expect(pl.sky_state().light_tint.g > clear_tint.g * 0.95f,
           "sky_test: with cloud shadows the cover no longer dims the sun uniformly (the shadows do)");
    cfg.cloud_shadows = false;
    expect(black_block_pixels(overcast) == 0, "sky_test: overcast has no black (NaN) blocks");

    // CPU zenith == GPU zenith: look straight up at a clear sky (no clouds, no sun in view), then
    // draw the gradient sky with the CPU's zenith colour through the same post chain.
    cfg.clouds = false;
    coopa::scene::SceneObject* cam = engine.scene().find_object("camera");
    if (cam && cam->get_transform()) cam->get_transform()->transform().set_rotation(glm::vec3(180.0f, 0.0f, 0.0f));
    tick_frames(engine, 2);
    const Frame up_phys = engine.capture_image(true);
    const glm::vec3 cpu_zenith = pl.render_config().indirect.sky_zenith;
    cfg.sky_model = "gradient";
    tick_frames(engine, 1);
    cfg.indirect.sky_zenith = cpu_zenith;
    tick_frames(engine, 2);
    const Frame up_grad = engine.capture_image(true);
    // The centre only: the gradient's colour changes away from straight up.
    const glm::vec3 a = band_mean(up_phys, 0.47f, 0.53f, 0.47f, 0.53f), b = band_mean(up_grad, 0.47f, 0.53f, 0.47f, 0.53f);
    expect(glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(6.0f))),
           "sky_test: the CPU zenith colour matches the GPU sky (gpu " + std::to_string(a.r) + " " + std::to_string(a.g) + " " +
               std::to_string(a.b) + ", cpu " + std::to_string(b.r) + " " + std::to_string(b.g) + " " + std::to_string(b.b) + ")");
}

/**
 * @brief The gradient sky costs nothing: with sky_model gradient no sky pass records (the
 *        tables never render) and toggling the physical sky on and back off returns the exact
 *        same image.
 */
COOPA_TEST(gradient_sky_costs_nothing_and_round_trips) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/tests/effects/weather_test/scene.yaml", 480, 270, 480, 270);
    config.render.aa_mode = "off";
    config.render.auto_exposure_enabled = false;
    config.render.shadows_enabled = false;   // its per-frame filter rotation: see test_physical_sky_renders
    toy::core::Engine engine(std::move(config));
    auto& pl = engine.pipeline();
    tick_frames(engine, 6);
    expect(!pl.physical_sky_active() && pl.render_config().sky_model == "gradient", "sky: the gradient sky is the default");
    const Frame before = engine.capture_image(true);
    auto& cfg = pl.render_config_mut();
    // Clouds need no physical sky: over the gradient they draw too, still with no sky pass.
    cfg.clouds = true;
    cfg.cloud_type = "volumetric";
    cfg.cloud_coverage = 0.6f;
    tick_frames(engine, 4);
    const Frame grad_clouds = engine.capture_image(true);
    const long long grad_cloud_px = count_diff(before, grad_clouds, 4);
    if (grad_cloud_px <= long(before.width * before.height) / 50) { dump_frame(before, "sky_grad_before"); dump_frame(grad_clouds, "sky_grad_clouds"); }
    expect(grad_cloud_px > long(before.width * before.height) / 50,
           "sky: volumetric clouds draw over the gradient sky (" + std::to_string(grad_cloud_px) + " px)");
    expect(black_block_pixels(grad_clouds) == 0, "sky: gradient-sky clouds have no black (NaN) blocks");
    expect(pl.sky_atmosphere_pass().lut_renders() == 0, "sky: the gradient sky records no sky pass");
    cfg.sky_model = "physical";
    tick_frames(engine, 3);
    const Frame phys = engine.capture_image(true);
    expect(count_diff(before, phys, 4) > long(phys.width * phys.height) / 10, "sky: the physical sky differs from the gradient");
    expect(black_block_pixels(phys) == 0, "sky: the physical sky has no black blocks");
    cfg.sky_model = "gradient";
    cfg.clouds = false;
    tick_frames(engine, 12);   // SSR reflects the previous frame's colour: let it settle
    const Frame after = engine.capture_image(true);
    const long long back_px = count_diff(before, after, 0);
    if (back_px != 0) { dump_frame(before, "sky_off_before"); dump_frame(after, "sky_off_after"); }
    expect(back_px == 0, "sky: switching back restores the gradient sky exactly (" + std::to_string(back_px) + " px)");
}

namespace {
/// A topdown test scene's engine: noon, cloudy, every per-frame filter that would make A/B
/// captures differ switched off.
std::unique_ptr<toy::core::Engine> topdown_engine(const char* scene) {
    toy::core::AppConfig config = make_test_config(scene, 480, 270, 480, 270);
    config.render.aa_mode = "off";
    config.render.auto_exposure_enabled = false;
    config.render.bloom_enabled = false;
    config.render.shadows_enabled = false;
    auto engine = std::make_unique<toy::core::Engine>(std::move(config));
    auto& cfg = engine->pipeline().render_config_mut();
    cfg.shadows_enabled = false;
    cfg.outline_enabled = false;
    if (toy::weather::WeatherSystem* w = engine->weather()) {
        w->set_time(11.0f);
        w->set_condition("cloudy", 0.0f);
    }
    tick_frames(*engine, 6);
    return engine;
}
float mean_luma(const Frame& f) { return glm::dot(band_mean(f, 0.0f, 1.0f), glm::vec3(1.0f / 3.0f)); }
}  // namespace

/**
 * @brief Low volumetric clouds under a topdown camera (topdown_sky_test: the gradient sky, a
 *        small cloud_scale at 45 m): they cover the ground zoomed out, fade away as the camera
 *        comes down (their shadows stay), cast shadows as dark as they are thick, and draw with
 *        the physical sky too.
 */
COOPA_TEST(low_volumetric_clouds_cover_fade_and_shadow) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    auto engine = topdown_engine("assets/scenes/tests/rendering/topdown_sky_test/scene.yaml");
    auto& pl = engine->pipeline();
    auto& cfg = pl.render_config_mut();
    expect(cfg.clouds && cfg.cloud_type == "volumetric" && cfg.cloud_shadows && cfg.cloud_camera_fade,
           "topdown: the scene has shadow-casting, height-faded volumetric clouds");
    expect(!pl.physical_sky_active(), "topdown: ...over the gradient sky");
    const long long px = long(480 * 270);

    // Zoomed out: the clouds (and their shadows) over the village.
    const Frame on = engine->capture_image(true);
    cfg.clouds = false;
    tick_frames(*engine, 12);   // the pond's SSR reflects the previous frame: let it settle
    const Frame off = engine->capture_image(true);
    const long long cloud_px = count_diff(on, off, 8);
    if (cloud_px < px / 20) { dump_frame(on, "topdown_vol_on"); dump_frame(off, "topdown_vol_off"); }
    expect(cloud_px > px / 20, "topdown: the low clouds show over the ground (" + std::to_string(cloud_px) + " px)");
    expect(black_block_pixels(on) == 0, "topdown: no black (NaN) blocks");

    // Faded out (the fade heights moved above the orbit camera, which owns its position): the
    // layer is gone and its march skipped, but the shadows stay and darken the ground.
    cfg.clouds = true;
    cfg.cloud_fade_start = 500.0f;
    cfg.cloud_fade_end = 600.0f;
    const uint64_t shadow_renders = pl.cloud_shadow_pass().renders();
    tick_frames(*engine, 3);
    expect(!pl.volumetric_clouds_traced(), "topdown: faded out, the march is skipped");
    expect(pl.cloud_shadow_pass().renders() > shadow_renders, "topdown: ...but the shadow map still renders");
    const Frame shadows = engine->capture_image(true);
    if (!(mean_luma(shadows) < mean_luma(off) - 1.0f)) { dump_frame(shadows, "topdown_vol_shadows"); dump_frame(off, "topdown_vol_off"); }
    expect(mean_luma(shadows) < mean_luma(off) - 1.0f, "topdown: cloud shadows darken the ground (" +
                                                           std::to_string(mean_luma(shadows)) + " vs " + std::to_string(mean_luma(off)) + ")");

    // Faded out with shadows off: the layer changes nothing at all. (12 frames: the pond's SSR
    // reflects the previous frame's colour, shadows and all, so let it settle.)
    cfg.cloud_shadows = false;
    tick_frames(*engine, 12);
    const long long faded_px = count_diff(engine->capture_image(true), off, 0);
    expect(faded_px == 0, "topdown: faded out and shadowless, the clouds change nothing (" + std::to_string(faded_px) + " px)");

    // Back out, physical sky: the same clouds draw there too.
    cfg.cloud_fade_start = 15.0f;
    cfg.cloud_fade_end = 60.0f;
    cfg.cloud_shadows = true;
    cfg.sky_model = "physical";
    tick_frames(*engine, 3);
    const Frame phys_on = engine->capture_image(true);
    cfg.clouds = false;
    tick_frames(*engine, 2);
    const Frame phys_off = engine->capture_image(true);
    expect(count_diff(phys_on, phys_off, 8) > px / 20, "topdown: the low clouds draw under the physical sky too");
    expect(black_block_pixels(phys_on) == 0, "topdown: physical sky, no black (NaN) blocks");
}

/**
 * @brief The flat (toon) clouds: from above (topdown_flat_clouds_test) they show zoomed out and
 *        cast shadows -- at opacity 0 the shadows alone remain -- and from below (sky_test, the
 *        layer raised over a ground camera) they draw as a deck across the sky.
 */
COOPA_TEST(flat_clouds_draw_from_above_and_below_and_shadow) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    const long long px = long(480 * 270);
    {
        auto engine = topdown_engine("assets/scenes/tests/rendering/topdown_flat_clouds_test/scene.yaml");
        auto& pl = engine->pipeline();
        auto& cfg = pl.render_config_mut();
        expect(cfg.clouds && cfg.cloud_type == "flat" && cfg.cloud_shadows, "flat: the scene has shadow-casting flat clouds");
        const Frame on = engine->capture_image(true);
        cfg.clouds = false;
        tick_frames(*engine, 2);
        const Frame off = engine->capture_image(true);
        const long long cloud_px = count_diff(on, off, 8);
        if (cloud_px < px / 20) { dump_frame(on, "flat_on"); dump_frame(off, "flat_off"); }
        expect(cloud_px > px / 20, "flat: the toon clouds show zoomed out (" + std::to_string(cloud_px) + " px)");
        expect(black_block_pixels(on) == 0, "flat: no black (NaN) blocks");

        cfg.clouds = true;
        cfg.flat_cloud_opacity = 0.0f;
        tick_frames(*engine, 2);
        const Frame shadows = engine->capture_image(true);
        expect(mean_luma(shadows) < mean_luma(off) - 1.0f, "flat: invisible clouds still shadow the ground (" +
                                                               std::to_string(mean_luma(shadows)) + " vs " + std::to_string(mean_luma(off)) + ")");
    }
    {
        toy::core::AppConfig config = make_test_config("assets/scenes/tests/rendering/sky_test/scene.yaml", 480, 270, 480, 270);
        config.render.aa_mode = "off";
        config.render.auto_exposure_enabled = false;
        config.render.bloom_enabled = false;
        config.render.shadows_enabled = false;
        toy::core::Engine engine(std::move(config));
        auto& cfg = engine.pipeline().render_config_mut();
        cfg.shadows_enabled = false;
        cfg.outline_enabled = false;
        cfg.sky_model = "gradient";
        cfg.cloud_type = "flat";
        cfg.cloud_altitude = 250.0f;
        cfg.cloud_thickness = 40.0f;
        cfg.flat_cloud_size = 150.0f;
        cfg.cloud_shadows = false;
        if (toy::weather::WeatherSystem* w = engine.weather()) {
            w->set_time(13.0f);
            w->set_condition("cloudy", 0.0f);
        }
        tick_frames(engine, 4);
        const Frame on = engine.capture_image(true);
        cfg.clouds = false;
        tick_frames(engine, 2);
        const Frame off = engine.capture_image(true);
        const long long deck_px = count_diff(on, off, 8);
        if (deck_px < px / 50) { dump_frame(on, "flat_below_on"); dump_frame(off, "flat_below_off"); }
        expect(deck_px > px / 50, "flat: seen from below, the deck draws across the sky (" + std::to_string(deck_px) + " px)");
        expect(black_block_pixels(on) == 0, "flat: from below, no black (NaN) blocks");
    }
}
