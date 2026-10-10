/**
 * @file snow_render_test.cpp
 * @brief snow_test end to end: cover, the precipitation map and the trench field reach the surface
 *        UBO, the shelter stays bare, the sled and the dropped ball dig tracks that refill, and the
 *        snow pattern rides with a moving object instead of staying world-fixed.
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
#include <toyengine/weather/weather_system.h>
#include <toyengine/world/snow_field.h>
#include <toyengine/world/snow_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("snow_render");

using namespace toy::test;

/** @brief snow_test: cover, the sheltered ground, trenches from the sled and the dropped ball. */
COOPA_TEST(snow_cover_trenches_and_shelter_reach_the_scene) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/tests/effects/snow_test/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 180);
    toy::weather::WeatherSystem* w = engine.weather();
    expect(w && w->state().enabled && w->state().snow_cover > 0.99f, "snow_test: it lies at full cover");
    expect_near(engine.pipeline().surface_world_ubo().snow.x, w ? w->state().snow_cover : 0.0f, 1e-5f,
                "snow_test: the cover reaches the surface world UBO");
    expect(engine.pipeline().surface_world_ubo().occl.w > 0.5f, "snow_test: ...with the precipitation map");
    const toy::world::SnowSystem* snow = toy::world::find_snow(engine.scene());
    expect(snow != nullptr, "snow_test: the snow system is installed");
    if (!snow || !w) return;
    expect(engine.pipeline().surface_world_ubo().field.w > 0.5f, "snow_test: ...and the trench field");
    expect(snow->depth_at({-6.0f, -6.0f}, 0.0f) > 0.25f, "snow_test: deep snow in the open");
    expect(snow->depth_at({4.0f, 3.0f}, 0.0f) < 0.05f, "snow_test: bare earth under the shelter");
    // The sled circles (-2, -2) at 2.5 m: some point of that circle is trenched.
    float deepest = 0.0f;
    for (int k = 0; k < 64; ++k) {
        const float a = static_cast<float>(k) / 64.0f * 6.2831853f;
        deepest = std::max(deepest, snow->trench_at(glm::vec2(-2.0f, -2.0f) + 2.5f * glm::vec2(std::cos(a), std::sin(a))));
    }
    expect(deepest > 0.15f, "snow_test: the sled ploughs a trench (" + std::to_string(deepest) + " m)");
    // ...that settles back behind it: the sled laps every 9 s and the scene's tracks recover in
    // 1.5 s, so the trench is a short trail, not a ring.
    int trenched = 0;
    for (int k = 0; k < 64; ++k) {
        const float a = static_cast<float>(k) / 64.0f * 6.2831853f;
        if (snow->trench_at(glm::vec2(-2.0f, -2.0f) + 2.5f * glm::vec2(std::cos(a), std::sin(a))) > 0.03f) ++trenched;
    }
    expect(trenched < 64 / 3, "snow_test: the trench fills back in behind the sled (" + std::to_string(trenched) + "/64 of the loop)");
    expect(w->settings().snow_patch_hard && engine.pipeline().surface_world_ubo().snow_style.x > 0.5f,
           "snow_test: hard-edged patches reach the surface world UBO");
    expect(snow->trench_at({2.5f, -2.5f}) > 0.05f, "snow_test: the dropped ball presses in (auto deformer)");
    expect(snow->trench_at({-6.0f, 5.0f}) == 0.0f, "snow_test: untouched snow stays untouched");

    // The cover layer on: forcing it off changes the frame a lot.
    const Frame snowy = engine.capture_image(true);
    engine.render_config().snow_cover_override = 0.0f;
    tick_frames(engine, 2);
    const Frame bare = engine.capture_image(true);
    expect_at_least(count_diff(snowy, bare, 12), snowy.width * snowy.height / 4, "snow_test: snow_cover_override 0 clears the snow");
    expect(engine.pipeline().surface_world_ubo().snow.x == 0.0f, "snow_test: ...through the UBO");
    engine.render_config().snow_cover_override = -1.0f;
}

/**
 * @brief The snow cover's pattern rides with a moving object (data::InstanceData::snow_anchor):
 *        move snow_test's crate and the camera together, and the crate's lid looks the same --
 *        where a pattern fixed in the world would have changed (and TAA would smear it).
 */
COOPA_TEST(snow_pattern_rides_with_a_moving_object) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/tests/effects/snow_test/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    config.render.snow_cover_override = 0.45f;   // partial cover: hard patches with edges on the lid
    toy::core::Engine engine(std::move(config));
    auto* crate = engine.scene().find_object("crate");
    auto* cc = engine.scene().find_first_component<toy::scene::CameraController>();
    toy::weather::WeatherSystem* w = engine.weather();
    expect(crate && cc && w && w->settings().snow_patch_hard, "snow ride: snow_test has the crate, a camera controller, hard patches");
    if (!crate || !cc || !w) return;

    const glm::vec3 start = crate->get_transform()->transform().position();
    // A move far enough that a WORLD-fixed pattern would put different snow on the lid: the CPU
    // mirror of the pattern, sampled over the lid at both places, must disagree substantially.
    const float size = w->settings().snow_patch_size;
    auto lid_mask = [&](const glm::vec3& centre) {
        std::vector<float> m;
        for (int j = -3; j <= 3; ++j) for (int i = -3; i <= 3; ++i)
            m.push_back(toy::world::snow_patch_mask(glm::vec2(centre) + glm::vec2(i, j) * 0.12f, 0.45f, 1.0f, size));
        return m;
    };
    glm::vec3 delta(0.0f);
    float best = 0.0f;
    for (float dx = 0.5f; dx <= 3.0f; dx += 0.25f) {
        const auto a = lid_mask(start), b = lid_mask(start + glm::vec3(dx, 0.0f, 0.0f));
        float d = 0.0f;
        for (size_t k = 0; k < a.size(); ++k) d += std::abs(a[k] - b[k]);
        d /= static_cast<float>(a.size());
        if (d > best) { best = d; delta = glm::vec3(dx, 0.0f, 0.0f); }
    }
    expect(best > 0.3f, "snow ride: a world-fixed pattern would change the lid's snow (" + std::to_string(best) + ")");

    cc->tracker            = "";
    cc->target             = start + glm::vec3(0.0f, 0.0f, 0.5f);
    cc->target_offset      = glm::vec3(0.0f);
    cc->distance           = 3.0f;
    cc->pitch_deg          = 60.0f;
    cc->follow_smoothing   = 0.0f;
    cc->movement_smoothing = 0.0f;
    tick_frames(engine, 30);
    const Frame before = engine.capture_image(true);

    crate->get_transform()->transform().set_position(start + delta);
    cc->target += delta;
    tick_frames(engine, 30);
    const Frame after = engine.capture_image(true);

    // The lid, around the image centre -- the orbit camera looks straight at it, and moved with
    // it, so these are the same lid pixels in both frames.
    const int cx = static_cast<int>(before.width / 2), cy = static_cast<int>(before.height / 2), r = 20;
    long long differ = 0, total = 0;
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(before.width) || y >= static_cast<int>(before.height)) continue;
            const size_t k = (static_cast<size_t>(y) * before.width + static_cast<size_t>(x)) * before.channels;
            ++total;
            for (int ch = 0; ch < 3; ++ch) {
                if (std::abs(static_cast<int>(before.pixels[k + ch]) - static_cast<int>(after.pixels[k + ch])) > 24) { ++differ; break; }
            }
        }
    }
    expect(total > 0 && differ * 20 < total,
           "snow ride: the lid's snow moved with the crate (" + std::to_string(differ) + "/" + std::to_string(total) + " px changed)");
    if (total == 0 || differ * 20 >= total) { dump_frame(before, "snow_ride_before"); dump_frame(after, "snow_ride_after"); }
}
