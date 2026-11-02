/**
 * @file heightmap_terrain_frame_budget_test.cpp
 * @brief The frame-time budget of a streaming HEIGHTMAP terrain: flying through one must not
 *        cost materially more per frame than standing still in it, mirroring
 *        terrain_frame_budget_test.cpp's tile-mode contract -- see that file's header for why
 *        this tier exists (a tuned threshold against wall-clock frame time) and why the bounds
 *        below are relative rather than absolute.
 *
 * Heightmap mode has no `ChunkBuildJob`: `stream_chunks_()` and `refresh_water_()` both run on
 * the owner thread, paced by `TerrainComponent::chunk_budget_ms` (see that field's doc comment
 * in terrain_component.h). This is the first automated measurement of whether that pacing
 * actually holds a flight's frame cost flat; before it, the only evidence was a one-off manual
 * `PROFILE=1` capture.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>
#include <gfxcoopa/engine/components/camera_component.h>
#include <toyengine/core/engine.h>
#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/heightmap_terrain_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("heightmap_terrain_frame_budget");

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

    toy::core::AppConfig config = make_test_config(heightmap_frame_test_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
                         "the demo registers a main camera");
    auto* camera_transform = camera->owner->get_transform();
    coopa::test::require(camera_transform != nullptr, "the camera has a transform");

    toy::world::HeightmapTerrain* terrain = camera->scene->find_first_component<toy::world::HeightmapTerrain>();
    coopa::test::require(terrain != nullptr, "the demo carries a HeightmapTerrain");

    // heightmap_frame_test_demo() erased the camera's CameraController, so this directly drives
    // the camera the way the tile test drives its (CameraController-tracked) focus marker.
    const float chunk = terrain->data != nullptr ? terrain->data->geometry().chunk_size : 64.0f;
    const float z      = camera_transform->transform().position().z;
    const auto park = [&](float x, float y) {
        camera_transform->transform().set_position(glm::vec3(x, y, z));
    };
    park(0.0f, 0.0f);
    tick_until(engine, 300, [&] { return terrain->runtime != nullptr && terrain->runtime->live_chunks > 0; });
    coopa::test::require(terrain->runtime != nullptr && terrain->runtime->live_chunks > 0,
                         "the world streams chunks in around the camera");
    for (int i = 0; i < 60; ++i) engine.tick();

    const auto time_frame = [&engine] {
        const auto t0 = std::chrono::steady_clock::now();
        engine.tick();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };

    // Standing still in a world that has finished streaming in: the reference the flight is
    // measured against, taken on the same machine in the same run.
    std::vector<double> still;
    still.reserve(240);
    for (int i = 0; i < 240; ++i) still.push_back(time_frame());

    // The same camera, flying a circle comfortably inside the fixture's view_distance (150 m):
    // the wanted-chunk window shifts under it every step, so chunks are cancelled, spawned and
    // their water refreshed the whole way, without the orbit itself straddling the view-distance
    // edge (which made every frame a boundary flicker rather than a steady trickle).
    std::vector<double> flight;
    flight.reserve(480);
    float angle = 0.0f;
    for (int step = 0; step < 480; ++step) {
        angle += 0.026f;
        park(1.75f * chunk * std::cos(angle), 1.75f * chunk * std::sin(angle));
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

    // Same relative shape as terrain_frame_budget_test.cpp's tile-mode contract: a ratio plus a
    // fixed allowance for streaming's own per-frame constant cost (see that file's comment for
    // why it is not a bare ratio), and p95/p99 bounds that catch a spawn or a water refresh
    // landing unbudgeted in one frame instead of spread across several.
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
