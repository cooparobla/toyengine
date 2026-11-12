/**
 * @file snow_test.cpp
 * @brief Lying snow, device-free: the cover model (accumulate, hold, melt), the trench field
 *        (stamp, refill, scroll, recover, view-following window, edge fade), hard-edged patches,
 *        and the open-sky / deep-snow CPU mirror under roofs. snow_render covers the GPU side.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/particles/particle_shape.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/world/snow_field.h>
#include <toyengine/world/snow_system.h>

#include "engine/support/checks.h"
#include "engine/support/weather_fixtures.h"

COOPA_TEST_SUITE("snow");

using namespace toy::test;

/** @brief Snow builds while it snows below freezing, holds in the cold, melts in the warmth. */
COOPA_TEST(cover_accumulates_holds_and_melts) {
    using toy::weather::WeatherSystem;
    expect(WeatherSystem::snows(0.5f, -4.0f) && !WeatherSystem::snows(0.5f, 6.0f) && !WeatherSystem::snows(0.0f, -4.0f),
           "snow: falls as snow only with precipitation below freezing");
    float c = 0.0f;
    for (int i = 0; i < 300; ++i) c = WeatherSystem::advance_snow_cover(c, 1.0f, -5.0f, 0.1f, 60.0f, 120.0f);
    expect_near(c, 0.5f, 1e-3f, "snow: 30 s of full snowfall at a 60 s accumulate_time -> half cover");
    for (int i = 0; i < 600; ++i) c = WeatherSystem::advance_snow_cover(c, 1.0f, -5.0f, 0.1f, 60.0f, 120.0f);
    expect_near(c, 1.0f, 1e-6f, "snow: ...and caps at full cover");
    const float held = WeatherSystem::advance_snow_cover(c, 0.0f, -2.0f, 30.0f, 60.0f, 120.0f);
    expect_near(held, 1.0f, 1e-6f, "snow: a dry frost keeps it");
    for (int i = 0; i < 600; ++i) c = WeatherSystem::advance_snow_cover(c, 0.0f, 5.0f, 0.1f, 60.0f, 120.0f);
    expect_near(c, 0.5f, 1e-3f, "snow: 60 s at +5 C melts half of a 120 s melt_time");
    const float rain = WeatherSystem::advance_snow_cover(0.5f, 1.0f, 5.0f, 10.0f, 60.0f, 120.0f);
    const float dry  = WeatherSystem::advance_snow_cover(0.5f, 0.0f, 5.0f, 10.0f, 60.0f, 120.0f);
    expect(rain < dry, "snow: rain on snow melts it faster");

    // The system: a snowy starting condition starts covered; settings round-trip.
    coopa::scene::Scene scene("weather");
    toy::weather::Settings st = toy::weather::parse_settings(weather_test_block(
        "condition: snowing\nsnow_max_depth: 0.4\nsnow_auto_deformers: true\n"
        "snow_trench_recover_time: 9\nsnow_patch_style: hard\nsnow_patch_size: 2.5\n"
        "conditions:\n"
        "  - {name: snowing, precipitation: 0.8, temperature: -6, transition: 10, duration: [5, 5]}\n"
        "  - {name: thaw, precipitation: 0.0, temperature: 9, transition: 1, duration: [5, 5]}\n"));
    expect(st.snow_auto_deformers && std::abs(st.snow_max_depth - 0.4f) < 1e-6f, "snow: settings parse");
    const toy::weather::Settings back = toy::weather::parse_settings(toy::weather::to_node(st));
    expect(back.snow_auto_deformers && std::abs(back.snow_max_depth - 0.4f) < 1e-6f &&
           std::abs(back.snow_accumulate_time - st.snow_accumulate_time) < 1e-4f, "snow: settings round-trip");
    expect(st.snow_patch_hard && std::abs(st.snow_patch_size - 2.5f) < 1e-6f && std::abs(st.snow_trench_recover_time - 9.0f) < 1e-6f,
           "snow: patch style, size and track recovery parse");
    expect(back.snow_patch_hard && std::abs(back.snow_patch_size - 2.5f) < 1e-6f && std::abs(back.snow_trench_recover_time - 9.0f) < 1e-6f,
           "snow: ...and round-trip");
    expect(!toy::weather::parse_settings(weather_test_block()).snow_patch_hard, "snow: soft patches by default");
    toy::weather::WeatherSystem w;
    w.set_settings(st);
    weather_step(w, scene, 1.0f);
    expect_near(w.state().snow_cover, 1.0f, 1e-6f, "snow: a snowy starting condition starts covered");
    expect_near(w.state().snow_depth, 0.4f, 1e-5f, "snow: depth = cover * snow_max_depth");
    w.set_condition("thaw", 0.0f);
    weather_step(w, scene, 60.0f, 0.5f);
    expect(w.state().snow_cover < 0.9f, "snow: it melts in the thaw (" + std::to_string(w.state().snow_cover) + ")");
    w.set_snow_cover(0.25f);
    expect_near(w.state().snow_cover, 0.25f, 1e-6f, "snow: set_snow_cover() sets it directly");
}

/** @brief SnowField: stamping (max-combine, falloff), refill, toroidal scrolling. */
COOPA_TEST(field_stamps_refills_and_scrolls) {
    toy::world::SnowField f(64, 0.1f, 1.0f);   // a 6.4 m window
    expect(f.trench_at({0.0f, 0.0f}) == 0.0f, "snow field: nothing before the first focus");
    f.set_focus({0.0f, 0.0f});
    const uint64_t v0 = f.version();
    f.stamp({1.0f, 1.0f}, 0.4f, 0.2f, 0.5f);
    expect(f.version() != v0, "snow field: a stamp bumps the version");
    expect_near(f.trench_at({1.0f, 1.0f}), 0.2f, 0.01f, "snow field: full depth at the centre");
    expect(f.trench_at({1.0f, 1.3f}) > 0.0f && f.trench_at({1.0f, 1.3f}) < 0.2f, "snow field: eased toward the rim");
    expect(f.trench_at({1.0f, 1.6f}) == 0.0f, "snow field: nothing past the radius");
    f.stamp({1.0f, 1.0f}, 0.4f, 0.1f, 0.5f);
    expect_near(f.trench_at({1.0f, 1.0f}), 0.2f, 0.01f, "snow field: a shallower stamp does not dig deeper (max)");
    f.refill(0.05f);
    expect_near(f.trench_at({1.0f, 1.0f}), 0.15f, 0.01f, "snow field: refill fills trenches in");
    // Scroll 2 m along +x: the trench (still inside) stays; cells that scrolled in are clean.
    f.set_focus({2.0f, 0.0f});
    expect_near(f.trench_at({1.0f, 1.0f}), 0.15f, 0.01f, "snow field: a trench still in the window survives a scroll");
    f.stamp({4.5f, 0.0f}, 0.3f, 0.25f, 0.0f);
    expect_near(f.trench_at({4.5f, 0.0f}), 0.25f, 0.01f, "snow field: stamps land in scrolled-in cells");
    // Back again: (4.5, 0) leaves the window (and its cells are reused for x < -1.2).
    f.set_focus({-2.0f, 0.0f});
    expect(f.trench_at({4.5f, 0.0f}) == 0.0f, "snow field: outside the window reads 0");
    expect(f.trench_at({-1.9f, 0.0f}) == 0.0f, "snow field: re-entered cells start clean (no toroidal ghost)");
    f.set_focus({500.0f, 0.0f});
    expect(f.trench_at({1.0f, 1.0f}) == 0.0f, "snow field: a jump past the window clears it");
}

/** @brief Tracks settle back over snow_trench_recover_time (no snowfall needed); the window
 *         follows where the camera looks; tracks fade out at the window's edge. */
COOPA_TEST(trenches_recover_and_the_window_follows_the_view) {
    coopa::scene::Scene scene("snow");
    toy::weather::WeatherSystem* w = toy::weather::install_weather_system(scene, weather_test_block(
        "snow_max_depth: 0.3\nsnow_trench_recover_time: 4\n"));
    w->set_snow_cover(1.0f);
    weather_step(*w, scene, 0.2f);
    toy::world::SnowSystem* snow = toy::world::install_snow_system(scene);
    auto step = [&](float seconds) {
        for (float t = 0.0f; t < seconds - 1e-4f; t += 0.01f) {
            coopa::scene::FrameContext ctx;
            ctx.delta_time = 0.01f;
            snow->execute(scene, ctx);
        }
    };
    toy::world::SnowField& f = snow->field();
    f.set_focus({0.0f, 0.0f});
    f.stamp({1.0f, 1.0f}, 0.4f, 0.3f, 0.0f);
    expect_near(f.trench_at({1.0f, 1.0f}), 0.3f, 0.01f, "snow recover: a full-depth track");
    step(2.0f);
    expect_near(f.trench_at({1.0f, 1.0f}), 0.15f, 0.02f, "snow recover: half filled at half the recover time (no snowfall)");
    step(2.2f);
    expect(f.trench_at({1.0f, 1.0f}) < 1e-3f, "snow recover: level again after the recover time");

    // The window centres on the ground point the camera looks at, within reach of the camera.
    using toy::world::SnowSystem;
    const glm::vec3 cam(0.0f, -20.0f, 20.0f);
    const glm::vec2 look = SnowSystem::focus_point(cam, glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)), 0.0f, 21.6f);
    expect(glm::distance(look, glm::vec2(0.0f, 0.0f)) < 1e-3f, "snow focus: where the view ray meets the ground");
    const glm::vec2 far = SnowSystem::focus_point(glm::vec3(0.0f, -40.0f, 5.0f), glm::normalize(glm::vec3(0.0f, 1.0f, -0.1f)), 0.0f, 21.6f);
    expect_near(glm::distance(far, glm::vec2(0.0f, -40.0f)), 21.6f, 1e-3f, "snow focus: kept within reach of the camera");
    const glm::vec2 level = SnowSystem::focus_point(cam, glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, 21.6f);
    expect(glm::distance(level, glm::vec2(cam)) < 1e-6f, "snow focus: the camera itself when it looks level");
    // Zooming an orbit camera out along its view keeps the target -- and its tracks -- in the window.
    toy::world::SnowField z(512, 0.1f, 1.0f);
    z.set_focus(SnowSystem::focus_point(glm::vec3(0.0f, -3.0f, 3.0f), glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)), 0.0f, 21.6f));
    z.stamp({0.0f, 0.0f}, 0.4f, 0.2f, 0.0f);
    z.set_focus(SnowSystem::focus_point(glm::vec3(0.0f, -28.0f, 28.0f), glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)), 0.0f, 21.6f));
    expect_near(z.trench_at({0.0f, 0.0f}), 0.2f, 0.01f, "snow focus: a track under the target survives zooming out to 40 m");

    // Edge fade: full inside, easing to nothing at the window edge (no step).
    toy::world::SnowField e(64, 0.1f, 1.0f);   // 3.2 m half-size
    e.set_focus({0.0f, 0.0f});
    for (float x = -3.1f; x <= 3.1f; x += 0.2f) e.stamp({x, 0.0f}, 0.15f, 0.2f, 0.0f);
    expect_near(e.trench_at({1.0f, 0.0f}), 0.2f, 0.01f, "snow edge: full depth well inside the window");
    const float near_edge = e.trench_at({3.0f, 0.0f});
    expect(near_edge > 0.0f && near_edge < 0.1f, "snow edge: faded near the window edge (" + std::to_string(near_edge) + ")");
    float prev = e.trench_at({2.4f, 0.0f}), worst = 0.0f;
    for (float x = 2.42f; x < 3.15f; x += 0.02f) { const float v = e.trench_at({x, 0.0f}); worst = std::max(worst, prev - v); prev = v; }
    expect(worst < 0.02f, "snow edge: the fade has no step (" + std::to_string(worst) + ")");
}

/** @brief Hard-edged patches (CPU mirror of gfx/surface/snow_patches.glsl) and deep snow in them. */
COOPA_TEST(hard_patches_grow_with_cover) {
    using toy::world::snow_patch_mask;
    const float size = 1.5f;
    auto coverage = [&](float cover, float receptive, int* soft_px = nullptr) {
        int in = 0, ramp = 0, n = 0;
        for (int j = 0; j < 120; ++j) for (int i = 0; i < 120; ++i, ++n) {
            const float m = snow_patch_mask({i * 0.17f - 10.0f, j * 0.17f + 3.0f}, cover, receptive, size);
            if (m > 0.5f) ++in;
            if (m > 0.0f && m < 1.0f) ++ramp;
        }
        if (soft_px) *soft_px = ramp;
        return static_cast<float>(in) / static_cast<float>(n);
    };
    expect(coverage(0.0f, 1.0f) < 0.02f, "snow patches: (almost) none at cover 0");
    expect(coverage(1.0f, 1.0f) > 0.999f, "snow patches: everything at full cover");
    float last = -1.0f;
    bool rising = true;
    for (float c = 0.1f; c <= 0.91f; c += 0.2f) { const float v = coverage(c, 1.0f); rising = rising && v >= last; last = v; }
    expect(rising, "snow patches: coverage grows with the cover");
    int ramp_px = 0;
    const float mid = coverage(0.45f, 1.0f, &ramp_px);
    expect(mid > 0.1f && mid < 0.9f, "snow patches: separate patches at middling cover (" + std::to_string(mid) + ")");
    // The ramp is the deep-snow mound's wall (the drawn colour edge is pixel-crisp on top of it):
    // a short slope, not a broad fade.
    expect(ramp_px < 120 * 120 * 35 / 100, "snow patches: a short edge ramp (" + std::to_string(ramp_px) + ")");
    expect(coverage(0.45f, 0.4f) < mid, "snow patches: shrink where less receptive (slopes, roof edges)");

    using toy::world::deep_snow_depth;
    const toy::world::SnowStyle hard{true, size};
    float inside = 0.0f, between = 0.0f;
    int n_in = 0, n_out = 0;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 p(i * 0.13f, 0.7f, 0.0f);
        const float m = snow_patch_mask(glm::vec2(p), 0.45f, 1.0f, size);
        const float d = deep_snow_depth(p, 0.3f, 0.45f, nullptr, nullptr, hard);
        if (m >= 1.0f) { inside = std::max(inside, d); ++n_in; }
        if (m <= 0.0f) { between = std::max(between, d); ++n_out; }
    }
    expect(n_in > 0 && n_out > 0, "deep snow (hard): the sample line crosses patches and gaps");
    expect_near(inside, 0.3f * 0.45f, 1e-4f, "deep snow (hard): full depth inside a patch");
    expect(between == 0.0f, "deep snow (hard): none between patches");
    expect_near(deep_snow_depth({3.3f, 0.7f, 0.0f}, 0.3f, 0.45f, nullptr, nullptr), 0.3f * 0.45f, 1e-4f, "deep snow (soft): even everywhere");
}

/** @brief The CPU open-sky / deep-snow mirror against a synthetic precipitation map with a roof. */
COOPA_TEST(open_sky_mirror_respects_roofs_and_trenches) {
    toy::particles::GroundField g;
    g.origin = {-8.0f, -8.0f};
    g.cell = 1.0f;
    g.nx = g.ny = 16;
    g.fallback = 0.0f;
    g.heights.assign(256, 0.0f);
    for (int j = 9; j < 12; ++j) for (int i = 9; i < 12; ++i) g.heights[static_cast<size_t>(j * 16 + i)] = 2.5f;   // roof over x,y in [1, 4)
    g.sky_heights = g.heights;
    g.heights[static_cast<size_t>(4 * 16 + 4)] = 1.0f;   // a crate passing over (-3.5, -3.5): heights only
    using toy::world::snow_open_sky;
    expect(snow_open_sky(&g, {-5.0f, -5.0f, 0.0f}, 0.5f) > 0.99f, "snow sky: open ground is open");
    expect(snow_open_sky(&g, {2.5f, 2.5f, 0.0f}, 0.5f) < 0.01f, "snow sky: ground under the roof is not");
    expect(snow_open_sky(&g, {2.5f, 2.5f, 2.5f}, 0.5f) > 0.99f, "snow sky: the roof's top is");
    expect(snow_open_sky(&g, {-3.5f, -3.5f, 0.0f}, 0.5f) > 0.99f, "snow sky: a moving body (heights, not sky_heights) does not shelter");
    expect(snow_open_sky(nullptr, {2.5f, 2.5f, 0.0f}, 0.5f) == 1.0f, "snow sky: no map = everything open");
    toy::world::SnowField trench(64, 0.1f, 1.0f);
    trench.set_focus({0.0f, 0.0f});
    trench.stamp({-2.0f, 0.0f}, 0.3f, 0.2f, 0.0f);
    using toy::world::deep_snow_depth;
    expect_near(deep_snow_depth({-5.0f, 0.0f, 0.0f}, 0.3f, 1.0f, &g, &trench), 0.3f, 1e-4f, "deep snow: full depth in the open");
    expect_near(deep_snow_depth({-5.0f, 0.0f, 0.0f}, 0.3f, 0.5f, &g, &trench), 0.15f, 1e-4f, "deep snow: scales with cover");
    expect_near(deep_snow_depth({-2.0f, 0.0f, 0.0f}, 0.3f, 1.0f, &g, &trench), 0.1f, 0.01f, "deep snow: less the trench");
    expect(deep_snow_depth({2.5f, 2.5f, 0.0f}, 0.3f, 1.0f, &g, &trench) < 1e-3f, "deep snow: none under the roof");
}
