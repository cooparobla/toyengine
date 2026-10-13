/**
 * @file terrain_churn_test.cpp
 * @brief A flight over a streaming tile world: chunks built, released and rebuilt for thousands
 *        of frames, with their colliders coming and going the whole way.
 *
 * The soak the shipped demo's own streaming shape produces -- 32-tile chunks, a wide view
 * radius, collision on -- and what it is here to catch is a use-after-free that only a world
 * destroying and rebuilding collider objects at this rate can reach: PhysicsSystem matched a
 * collider group to its existing body by Collider POINTER, so a chunk collider allocated into
 * the block a destroyed one had just freed looked like the same collider, and the binding kept
 * for it still named the destroyed SceneObject as the object whose Transform drives the body.
 * prime_transforms_() then read a freed object (see Collider::instance_id()).
 *
 * Nothing here asserts on the crash directly -- there is no state to inspect, the process
 * simply dies. What makes the test one is the length of the flight and `collision = true`.
 * Colliders off, or small chunks, and it passes with the bug in place.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_churn");

using namespace toy::test;

COOPA_TEST(streaming_chunks_for_thousands_of_frames) {
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

    // A small world with small chunks and a tight view radius: the streamer then builds and
    // drops a whole ring of chunks for every hop below, which is the churn under test.
    // The shipped demo's streaming shape (32-tile chunks, a wide view radius, collision on),
    // on a world small enough to generate in seconds: ten chunks per axis, so a lap of the
    // circuit below builds and drops most of them.
    terrain->generator.grid_size        = 80;
    terrain->params.tiles_per_grid_unit = 4;
    terrain->params.chunk_size          = 32;
    terrain->params.view_radius         = 4;
    terrain->collision                  = true;   // the colliders are the point: see the file doc
    terrain->max_chunk_jobs_per_frame   = 4;

    const float chunk_world = terrain->params.chunk_world_size();
    coopa::scene::SceneObject* marker = scene.find_object_by_path(camera->focus_object);
    coopa::test::require(marker != nullptr && marker->get_transform() != nullptr, "the focus marker is there");
    auto* marker_transform = marker->get_transform();
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 2.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
        controller->pitch_deg          = controller->max_pitch_deg;
    }
    const auto park = [&](int cx, int cy) {
        marker_transform->transform().set_position(
            glm::vec3((static_cast<float>(cx) + 0.5f) * chunk_world,
                      (static_cast<float>(cy) + 0.5f) * chunk_world, 48.0f));
    };
    park(5, 5);
    tick_until(engine, 2400, [&] { return count_live_chunks(*terrain) >= 81; });
    coopa::test::require(terrain->is_ready(), "the world comes up");
    std::printf("  up: %d live chunks, %zu tracked\n", count_live_chunks(*terrain), terrain->chunks().size());

    // A circuit of hops, each a few chunks: every hop drops a ring behind and builds one ahead.
    // Flown, not teleported: the marker crosses the world at a few tiles a frame, so the
    // streamer is cancelling, retiring and submitting chunks continuously -- and a hop over
    // the diagonal every lap drops the whole loaded region at once.
    const int laps = 4;
    std::size_t peak_tracked = 0;
    float angle = 0.0f;
    for (int lap = 0; lap < laps; ++lap) {
        for (int step = 0; step < 240; ++step) {
            angle += 0.026f;
            const float r = 3.2f;
            marker_transform->transform().set_position(
                glm::vec3((5.0f + r * std::cos(angle)) * chunk_world,
                          (5.0f + r * std::sin(angle)) * chunk_world, 48.0f));
            engine.tick();
            peak_tracked = std::max(peak_tracked, terrain->chunks().size());
        }
        park(lap % 2 == 0 ? 1 : 8, lap % 2 == 0 ? 8 : 1);   // a jump: every chunk in view is released
        for (int i = 0; i < 60; ++i) engine.tick();
        peak_tracked = std::max(peak_tracked, terrain->chunks().size());
        std::printf("  lap %d: %d live, %zu tracked (peak %zu)\n", lap + 1, count_live_chunks(*terrain),
                    terrain->chunks().size(), peak_tracked);
    }
    tick_until(engine, 1200, [&] { return count_live_chunks(*terrain) >= 40; });
    expect(terrain->chunks().size() < 400, "the tracked chunk set stays bounded (" +
                                               std::to_string(terrain->chunks().size()) + ")");
    expect(count_live_chunks(*terrain) >= 40, "the world is still streaming after the circuit");
}
