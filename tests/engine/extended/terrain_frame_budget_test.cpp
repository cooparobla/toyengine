/**
 * @file terrain_frame_budget_test.cpp
 * @brief The frame-time budget of a streaming tile world: flying through one must not cost
 *        materially more per frame than standing still in it.
 *
 * Extended rather than gpu, and for the reason this tier exists: the threshold is tuned and the
 * measurement is wall-clock frame time, which on a desktop is not entirely this suite's to
 * control. The same flight measures p99 at 1.5x the median on a quiet machine and 2.5x when
 * something else wakes up. Run it when working on the streamer; do not gate every change on it.
 *
 * What it is calibrated against (the shape below; the first two rows 2026-10-12 on the then
 * -O0 dev build, the third 2026-10-13 on the RelWithDebInfo default with the Scene's cached
 * component queries):
 *
 * | | median | p95 | p99 | max |
 * |---|---|---|---|---|
 * | centring on the camera's view RAY, chunk mesh built in the frame it lands | 14.2 | 35.8 | 37.1 | 40.3 |
 * | latched centre, mesh + collider + buffers all on the worker | 16.7 | 24.1 | 25.5 | 28.3 |
 * | + cached scene queries, -O2 build | 16.7 | 17.3 | 17.8 | 18.4 |
 *
 * The median is pinned at ~16.7 by 60 Hz presentation pacing; what the fixes removed is the
 * tail. The ratios are what matter -- p95 2.52x -> 1.44x -> 1.04x of the median -- which is why
 * the bounds below are relative.
 *
 * The deterministic halves of the same contract (no churn when looking around, no hole under the
 * camera, the settled frame) are asserted in tests/engine/gpu/terrain_stream_test.cpp, which
 * runs on every change.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_frame_budget");

using namespace toy::test;

namespace {

double percentile(std::vector<double> xs, double q) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    return xs[static_cast<std::size_t>(q * static_cast<double>(xs.size() - 1))];
}

} // namespace

COOPA_TEST(a_flight_costs_no_more_per_frame_than_standing_still) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
                         "the demo registers a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    coopa::test::require(terrain != nullptr, "the demo carries a TileTerrain");
    // terrain_churn_test's streaming shape: 32-tile chunks, collision on -- the one that churns
    // chunks and their colliders continuously.
    terrain->generator.grid_size        = 80;
    terrain->params.tiles_per_grid_unit = 4;
    terrain->params.chunk_size          = 32;
    terrain->params.view_radius         = 4;
    terrain->collision                  = true;

    const float chunk_world = terrain->params.chunk_world_size();
    coopa::scene::SceneObject* marker = scene.find_object_by_path(camera->focus_object);
    coopa::test::require(marker != nullptr && marker->get_transform() != nullptr, "the focus marker is there");
    auto* marker_transform = marker->get_transform();
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 2.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
        controller->pitch_deg          = 20.0f;
    }
    const auto park = [&](float cx, float cy) {
        marker_transform->transform().set_position(glm::vec3(cx * chunk_world, cy * chunk_world, 48.0f));
    };
    park(5.5f, 5.5f);
    tick_until(engine, 2400, [&] { return count_live_chunks(*terrain) >= 70; });
    coopa::test::require(terrain->is_ready(), "the world comes up");
    for (int i = 0; i < 60; ++i) engine.tick();

    const auto time_frame = [&engine] {
        const auto t0 = std::chrono::steady_clock::now();
        engine.tick();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };

    // Standing still in a world that is entirely built: the reference the flight is measured
    // against, taken on the same machine in the same run so a slow GPU or a busy desktop moves
    // both halves together.
    std::vector<double> still;
    still.reserve(240);
    for (int i = 0; i < 240; ++i) still.push_back(time_frame());

    // The same camera, flying: chunks cancelled, retired, built and published the whole way.
    std::vector<double> flight;
    flight.reserve(480);
    float angle = 0.0f;
    for (int step = 0; step < 480; ++step) {
        angle += 0.026f;
        park(5.0f + 3.2f * std::cos(angle), 5.0f + 3.2f * std::sin(angle));
        flight.push_back(time_frame());
    }

    const double still_median  = percentile(still, 0.5);
    const double flight_median = percentile(flight, 0.5);
    const double flight_p95    = percentile(flight, 0.95);
    const double flight_p99    = percentile(flight, 0.99);
    const double flight_max    = *std::max_element(flight.begin(), flight.end());
    std::printf("  still:  median %.2f ms, p95 %.2f ms\n", still_median, percentile(still, 0.95));
    std::printf("  flight: median %.2f ms, p95 %.2f ms, p99 %.2f ms, max %.2f ms\n", flight_median,
                flight_p95, flight_p99, flight_max);

    // A ratio PLUS a fixed allowance, because streaming's own cost is a per-frame constant, not
    // a proportion of the frame: measured here it is +0.46 ms (culling more chunks in view ~0.10,
    // the snow field ~0.10, the rest spread over the scene update). The bare 1.25x ratio was
    // calibrated when a headless frame was vsync-paced at 16.7 ms and that 0.46 ms was 3% of it;
    // with presentation out of the measurement (vsync off is IMMEDIATE now) the same work is 20%
    // of a 2 ms frame. The allowance is what a frame can carry without being felt; the ratio
    // still scales it for a slower machine, and the p95/p99 checks below are what catch a bake
    // or an upload landing unbudgeted in one frame.
    expect(flight_median < 1.25 * still_median + 0.75,
           "flying costs about what standing still does (" + std::to_string(flight_median) + " vs " +
               std::to_string(still_median) + " ms)");
    expect(flight_p95 < 1.75 * flight_median,
           "and the common frame is flat (p95 " + std::to_string(flight_p95) + " ms vs median " +
               std::to_string(flight_median) + " ms)");
    expect(flight_p99 < 2.5 * flight_median,
           "with no sustained hitches (p99 " + std::to_string(flight_p99) + " ms vs median " +
               std::to_string(flight_median) + " ms)");
}
