/**
 * @file terrain_stream_test.cpp
 * @brief The streaming contract of a tile world: looking around must not reload it, and flying
 *        through it must not hitch.
 *
 * Both halves measure the streamer from the outside -- the chunk table and the wall clock -- so
 * neither depends on TerrainSystem's internals.
 *
 *  - `looking_around_keeps_its_chunks`: the loaded region follows where the camera *is*, not
 *    where its view ray happens to land. An orbit camera spun through a full turn at a shallow
 *    pitch sweeps that ray across hundreds of metres; a region centred on the ray drops and
 *    rebuilds whole rings for a camera that never moved.
 *  - `a_rebuilt_chunk_keeps_its_water`: a chunk re-meshed for an edit must not take its lakes
 *    and rivers away while it does. A chunk's water is a WaterBody the water system bakes over
 *    a frame or two, so re-creating one leaves the bed showing through for those frames.
 *  - `crossing_the_collision_radius_only_costs_a_collider`: the other half of the same contract.
 *    A chunk coming inside `collision_radius` needs a collision mesh and nothing else, so it
 *    must not be republished to get one.
 *  - `a_flight_keeps_its_work_off_the_frame`: a chunk arriving must not cost the frame it lands
 *    in. Everything about a chunk -- meshing, welding, the collider's BVH, the GPU buffers -- is
 *    built on a worker, so what is asserted is the owner thread's share of it. The frame
 *    percentiles are printed alongside; the budget on them lives in
 *    tests/engine/extended/terrain_frame_budget_test.cpp, because wall-clock frame time on a
 *    desktop is not this suite's to control.
 *
 * The flight shape is terrain_churn_test's (32-tile chunks, collision on): the one that churns
 * chunks and their colliders continuously.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/scene/camera_controller.h>
#include <physxcoopa/components/mesh_collider.h>

#include <toyengine/water/water_body.h>
#include <toyengine/water/water_system.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/terrain_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_stream");

using namespace toy::test;

namespace {

using ChunkSet = std::unordered_set<toy::world::ChunkCoord>;   // tile_types.h hashes the key

/** @brief Every coordinate the terrain is tracking, whatever state its chunk is in. */
ChunkSet tracked(const toy::world::TerrainComponent& terrain) {
    ChunkSet out;
    for (const auto& entry : terrain.chunks()) out.insert(entry.first);
    return out;
}

/** @brief A terrain shrunk to the churn test's streaming shape, parked in the middle. */
toy::world::TerrainComponent& prepare(coopa::scene::Scene& scene) {
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    coopa::test::require(terrain != nullptr, "the demo carries a TileTerrain");
    terrain->generator.grid_size        = 80;
    terrain->params.tiles_per_grid_unit = 4;
    terrain->params.chunk_size          = 32;
    terrain->params.view_radius         = 4;
    terrain->collision                  = true;
    return *terrain;
}

double percentile(std::vector<double> xs, double q) {
    if (xs.empty()) return 0.0;
    std::sort(xs.begin(), xs.end());
    const std::size_t i = static_cast<std::size_t>(q * static_cast<double>(xs.size() - 1));
    return xs[i];
}

} // namespace

COOPA_TEST(looking_around_keeps_its_chunks) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
                         "the demo registers a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    toy::world::TerrainComponent& terrain = prepare(scene);

    const float chunk_world = terrain.params.chunk_world_size();
    coopa::scene::SceneObject* marker = scene.find_object_by_path(camera->focus_object);
    coopa::test::require(marker != nullptr && marker->get_transform() != nullptr, "the focus marker is there");
    marker->get_transform()->transform().set_position(glm::vec3(5.5f * chunk_world, 5.5f * chunk_world, 48.0f));

    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    coopa::test::require(controller != nullptr, "the demo camera is an orbit rig");
    controller->distance           = 2.0f;
    controller->follow_smoothing   = 0.0f;
    controller->movement_smoothing = 0.0f;
    // Shallow, so the view ray reaches far across the world -- the case a ray-centred region
    // cannot survive. Steeply down, the ray lands under the camera and proves nothing.
    controller->pitch_deg          = 15.0f;
    controller->yaw_deg            = 0.0f;

    tick_until(engine, 2400, [&] { return count_live_chunks(terrain) >= 70; });
    coopa::test::require(terrain.is_ready(), "the world comes up");
    for (int i = 0; i < 90; ++i) engine.tick();   // let the region settle completely

    const ChunkSet before = tracked(terrain);
    std::printf("  settled: %d live, %zu tracked\n", count_live_chunks(terrain), before.size());

    // A full turn in four seconds, the camera itself standing still.
    controller->auto_rotate_deg_per_sec = 90.0f;
    ChunkSet dropped;
    ChunkSet live_now = before;
    for (int i = 0; i < 260; ++i) {
        engine.tick();
        const ChunkSet now = tracked(terrain);
        for (const auto& coord : live_now) {
            if (now.find(coord) == now.end()) dropped.insert(coord);
        }
        live_now = now;
    }
    controller->auto_rotate_deg_per_sec = 0.0f;

    const ChunkSet after = tracked(terrain);
    std::size_t added = 0;
    for (const auto& coord : after) added += before.find(coord) == before.end() ? 1 : 0;
    std::printf("  after a full turn: %zu chunks dropped, %zu added, %zu tracked\n",
                dropped.size(), added, after.size());

    // A handful of chunks may legitimately come and go at the edge (the camera orbits a little
    // as it yaws). A whole ring is 9 chunks a side at this radius; dropping more than that means
    // the region is following the view ray.
    expect(dropped.size() <= 8, "looking around drops almost nothing (" + std::to_string(dropped.size()) + ")");
    expect(added <= 8, "looking around builds almost nothing new (" + std::to_string(added) + ")");
}

namespace {

/** @brief Per chunk, what a streaming test watches change: its published serial and its water. */
struct ChunkWatch {
    std::uint32_t serial    = 0;
    bool          water     = false;   ///< It carries at least one WaterBody.
    bool          published = false;   ///< Every one of them has a mesh on screen.
    bool          collider  = false;
};

/**
 * @brief Folds this frame's chunks into `seen`, which REMEMBERS one through its rebuild.
 *
 * A chunk being re-meshed is Meshing, not Live, so it drops out of a frame's snapshot for as
 * long as its job runs -- and a straight snapshot-to-snapshot comparison would never see across
 * the gap, which is exactly where a rebuild's before and after are. Only a chunk the terrain has
 * stopped tracking altogether is forgotten.
 */
void merge_watch(std::unordered_map<toy::world::ChunkCoord, ChunkWatch>& seen,
                 const std::unordered_map<toy::world::ChunkCoord, ChunkWatch>& now,
                 const toy::world::TerrainComponent& terrain) {
    for (auto it = seen.begin(); it != seen.end();) {
        it = terrain.chunks().find(it->first) == terrain.chunks().end() ? seen.erase(it) : std::next(it);
    }
    for (const auto& [coord, state] : now) seen[coord] = state;
}

/** @brief Every chunk that is Live with an object, as ChunkWatch sees it. */
std::unordered_map<toy::world::ChunkCoord, ChunkWatch> watch_chunks(const toy::world::TerrainComponent& terrain) {
    std::unordered_map<toy::world::ChunkCoord, ChunkWatch> out;
    for (const auto& [coord, chunk] : terrain.chunks()) {
        if (chunk.state != toy::world::ChunkState::Live || chunk.object == nullptr) continue;
        ChunkWatch w;
        w.serial    = chunk.mesh_serial;
        w.published = true;
        w.collider  = chunk.object->get_component<coopa::physx::components::MeshCollider>() != nullptr;
        for (const auto& child : chunk.object->children()) {
            auto* body = child->get_component<toy::water::WaterBody>();
            if (body == nullptr) continue;
            w.water = true;
            if (!toy::water::WaterSystem::is_published(*body)) w.published = false;
        }
        out.emplace(coord, w);
    }
    return out;
}

} // namespace

/**
 * @brief A chunk re-meshed for an edit must keep its water drawing the whole way through.
 *
 * Painting a surface kind changes what a chunk draws and nothing else -- it moves no ground, so
 * it moves no water -- but it marks the chunk stale, and a stale chunk is rebuilt by replacing
 * its SceneObject. Its terrain mesh is swapped in the frame the rebuild lands and never blinks;
 * its water children are bare WaterBodies, and toy::water::WaterSystem bakes and publishes those
 * over the following frame or two -- so re-creating them blinks: a lake or a river vanishes for
 * a frame and the bed under it shows through, which at a distance reads as chunks flashing.
 *
 * Water the rebuild has not changed is therefore carried across instead. What is asserted is the
 * invariant that makes the blink impossible: while a chunk keeps an object on the tree, water it
 * has already published stays published. `rebuilt` is checked too -- without rebuilds the edits
 * would prove nothing.
 */
COOPA_TEST(a_rebuilt_chunk_keeps_its_water) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
                         "the demo registers a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    toy::world::TerrainComponent& terrain = prepare(scene);

    const float chunk_world = terrain.params.chunk_world_size();
    coopa::scene::SceneObject* marker = scene.find_object_by_path(camera->focus_object);
    coopa::test::require(marker != nullptr && marker->get_transform() != nullptr, "the focus marker is there");
    marker->get_transform()->transform().set_position(glm::vec3(5.5f * chunk_world, 5.5f * chunk_world, 48.0f));
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 2.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
        controller->pitch_deg          = 20.0f;
    }
    tick_until(engine, 2400, [&] { return count_live_chunks(terrain) >= 70; });
    coopa::test::require(terrain.is_ready(), "the world comes up");
    for (int i = 0; i < 120; ++i) engine.tick();   // every first bake settles

    // The chunks worth editing are the ones with water on them. An archipelago has plenty.
    std::vector<toy::world::ChunkCoord> watery;
    for (const auto& [coord, w] : watch_chunks(terrain)) {
        if (w.water) watery.push_back(coord);
    }
    std::sort(watery.begin(), watery.end(), [](const auto& a, const auto& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;   // deterministic, whatever the map order was
    });
    coopa::test::require(watery.size() >= 4, "the loaded region has water on it (" +
                         std::to_string(watery.size()) + " chunks)");

    std::unordered_map<toy::world::ChunkCoord, ChunkWatch> seen = watch_chunks(terrain);
    int holes = 0, rebuilt = 0, watched = 0;
    const auto sample = [&] {
        auto now = watch_chunks(terrain);
        for (const auto& [coord, state] : now) {
            const auto was = seen.find(coord);
            if (was == seen.end() || !was->second.water || !state.water) continue;
            if (state.serial > was->second.serial) ++rebuilt;
            if (!was->second.published) continue;
            ++watched;
            if (!state.published) ++holes;
        }
        merge_watch(seen, now, terrain);
    };

    // Paint the middle of each watery chunk, one per pass, alternating kinds so every pass is a
    // real edit; then let the rebuild land and watch what the chunk does with its water.
    const std::int32_t size = terrain.params.chunk_size;
    for (int pass = 0; pass < 4; ++pass) {
        for (const toy::world::ChunkCoord& coord : watery) {
            const std::int32_t x0 = coord.x * size + size / 4;
            const std::int32_t y0 = coord.y * size + size / 4;
            terrain.paint_tiles(toy::world::IRect{x0, y0, x0 + size / 2, y0 + size / 2},
                                pass % 2 == 0 ? toy::world::TileKind::Stone : toy::world::TileKind::Dirt,
                                /*side=*/false);
        }
        for (int i = 0; i < 40; ++i) {
            engine.tick();
            sample();
        }
    }

    std::printf("  %d chunk rebuilds over %d watched chunk-frames, %d lost their water\n",
                rebuilt, watched, holes);
    expect(rebuilt > 0, "the edits rebuilt chunks (" + std::to_string(rebuilt) + ")");
    expect(watched > 1000, "the passes watched water (" + std::to_string(watched) + " chunk-frames)");
    expect(holes == 0, "no chunk ever gives its water back unpublished (" + std::to_string(holes) + ")");
}

/**
 * @brief Coming inside `collision_radius` costs a chunk a collider and nothing else.
 *
 * A chunk crossing the threshold needs a collision mesh it was not built with, and nothing
 * whatever besides: its surface, its LOD levels and its water are all still right. Rebuilding
 * the chunk to get one re-meshes it, re-uploads its vertex and index buffers, republishes its
 * asset and swaps its SceneObject -- which also throws away everything the renderer keys on that
 * object's MeshRenderer, its motion vectors and its LOD hysteresis among them. The collider-only
 * pass (ChunkBuildJob::collision_only) builds the shape on a worker and hangs it on the object
 * the chunk is already drawing from.
 *
 * `mesh_serial` is bumped once per publish, so it is exactly the thing that must NOT move.
 */
COOPA_TEST(crossing_the_collision_radius_only_costs_a_collider) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
                         "the demo registers a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    toy::world::TerrainComponent& terrain = prepare(scene);
    // Well inside the view radius, so chunks cross it in the middle of the loaded region rather
    // than at its edge, where they would be arriving (and publishing) anyway.
    terrain.collision_radius = 2;

    const float chunk_world = terrain.params.chunk_world_size();
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
    tick_until(engine, 2400, [&] { return count_live_chunks(terrain) >= 70; });
    coopa::test::require(terrain.is_ready(), "the world comes up");
    for (int i = 0; i < 120; ++i) engine.tick();

    std::unordered_map<toy::world::ChunkCoord, ChunkWatch> seen = watch_chunks(terrain);
    int gained = 0, republished = 0, lost_water = 0;
    const auto sample = [&] {
        auto now = watch_chunks(terrain);
        for (const auto& [coord, state] : now) {
            const auto was = seen.find(coord);
            if (was == seen.end()) continue;
            if (state.serial > was->second.serial) ++republished;
            if (state.collider && !was->second.collider) ++gained;
            if (was->second.water && was->second.published && state.water && !state.published) ++lost_water;
        }
        merge_watch(seen, now, terrain);
    };

    // The churn circuit: chunks cross the collision threshold in both directions the whole way.
    float angle = 0.0f;
    for (int step = 0; step < 480; ++step) {
        angle += 0.026f;
        park(5.0f + 3.2f * std::cos(angle), 5.0f + 3.2f * std::sin(angle));
        engine.tick();
        sample();
    }

    // What the flight ended on: inside the radius is collidable, outside is not.
    const toy::world::ChunkCoord centre = terrain.params.chunk_at(
        marker_transform->transform().get_world_matrix()[3]);
    int inside = 0, inside_collidable = 0;
    for (const auto& [coord, state] : watch_chunks(terrain)) {
        if (!terrain_wants_collision(terrain, coord, centre)) continue;
        ++inside;
        inside_collidable += state.collider ? 1 : 0;
    }

    std::printf("  %d colliders gained, %d chunks republished, %d of %d inside the radius collide\n",
                gained, republished, inside_collidable, inside);
    expect(gained > 0, "chunks crossed into the radius (" + std::to_string(gained) + " colliders gained)");
    expect(republished == 0, "crossing the radius republishes nothing (" + std::to_string(republished) + ")");
    expect(lost_water == 0, "and takes no chunk's water away (" + std::to_string(lost_water) + ")");
    expect(inside > 4 && inside_collidable == inside, "every chunk inside the radius collides (" +
           std::to_string(inside_collidable) + " of " + std::to_string(inside) + ")");
}

COOPA_TEST(a_flight_keeps_its_work_off_the_frame) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
                         "the demo registers a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    toy::world::TerrainComponent& terrain = prepare(scene);

    const float chunk_world = terrain.params.chunk_world_size();
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
    tick_until(engine, 2400, [&] { return count_live_chunks(terrain) >= 70; });
    coopa::test::require(terrain.is_ready(), "the world comes up");
    for (int i = 0; i < 60; ++i) engine.tick();

    auto* system = dynamic_cast<toy::world::TerrainSystem*>(scene.find_system("Terrain"));
    const std::uint64_t built_before = system != nullptr ? system->chunks_built() : 0;
    double publish_ms = 0.0;   // what landing a chunk costs the owner thread, summed

    // A lap of the circuit: a few tiles a frame, so chunks are cancelled, retired, built and
    // uploaded continuously the whole way.
    std::vector<double> ms;
    ms.reserve(960);
    float angle = 0.0f;
    int holes = 0;
    for (int lap = 0; lap < 2; ++lap) {
        for (int step = 0; step < 240; ++step) {
            angle += 0.026f;
            park(5.0f + 3.2f * std::cos(angle), 5.0f + 3.2f * std::sin(angle));
            const auto t0 = std::chrono::steady_clock::now();
            engine.tick();
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            if (system != nullptr) publish_ms += system->last_upload_ms();
            // The ground the camera is over must never be missing: a region that follows
            // anything but the camera leaves exactly this hole, and it is the one a player
            // falls through.
            const glm::vec3 eye(camera->owner->get_transform()->transform().get_world_matrix()[3]);
            if (!chunk_is_live(terrain, terrain.params.chunk_at(eye))) ++holes;
        }
    }
    std::printf("  %d of %zu frames had no live chunk under the camera\n", holes, ms.size());
    expect(holes == 0, "the ground under the camera is loaded the whole way round");

    // What the flight actually kept up: a streamer that drops its world and rebuilds it is
    // cheap per frame for the wrong reason, so the frame times below only mean something
    // alongside these.
    const int built    = system != nullptr ? static_cast<int>(system->chunks_built()) : -1;
    const int released = system != nullptr ? static_cast<int>(system->chunks_released()) : -1;
    std::printf("  flight kept %d live chunks (%zu tracked); %d chunk meshes built, %d released\n",
                count_live_chunks(terrain), terrain.chunks().size(), built, released);
    // What is irreducibly on the owner thread: publishing the asset, the scene insert, the water
    // children. Everything else about a chunk happens on a worker.
    const int landed = system != nullptr ? static_cast<int>(system->chunks_built() - built_before) : 0;
    std::printf("  publishing cost %.1f ms over %zu frames (%.3f ms/frame, %.3f ms per chunk landed, %d landed)\n",
                publish_ms, ms.size(), publish_ms / static_cast<double>(ms.size()),
                landed > 0 ? publish_ms / landed : 0.0, landed);

    const double median = percentile(ms, 0.5);
    const double p95    = percentile(ms, 0.95);
    const double p99    = percentile(ms, 0.99);
    const double worst  = *std::max_element(ms.begin(), ms.end());
    std::printf("  frames %zu: median %.2f ms, p95 %.2f ms, p99 %.2f ms, max %.2f ms (%.1fx median)\n",
                ms.size(), median, p95, p99, worst, worst / std::max(median, 1e-3));

    // The frame percentiles are PRINTED, not asserted. Wall-clock frame time on a desktop is
    // not this suite's to control: the same flight measures p99 at 1.5x the median on a quiet
    // machine and 2.5x when something else wakes up, whatever the streamer does. The budget
    // lives in tests/engine/extended/terrain_frame_budget_test.cpp, where tuned thresholds
    // belong; what is asserted here is the thing the engine actually owns -- how much of a
    // chunk's arrival is spent on the owner thread.
    //
    // 0.15 ms is a rounding error against a 16 ms frame and still ~5x the measured cost, so it
    // fails on a regression (someone moving the mesh build, the collider or the water bake back
    // into the frame the chunk lands in) rather than on a noisy machine.
    expect(landed > 100, "the flight actually landed chunks (" + std::to_string(landed) + ")");
    if (landed > 0) {
        const double per_chunk = publish_ms / landed;
        expect(per_chunk < 0.15, "publishing a chunk stays off the frame (" + std::to_string(per_chunk) +
                                     " ms per chunk)");
    }

    // Move, then stop and hold: what the flight leaves on screen once everything has settled.
    // A streamer that recentres on anything but the camera shows it here as sky where the
    // ground under the camera should be.
    for (int i = 0; i < 90; ++i) engine.tick();
    const Frame frame = engine.capture_image(/*low_res=*/true);
    long long ground = 0, total = 0;
    for (std::uint32_t y = frame.height / 2; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * frame.width + x) * frame.channels;
            const int r = frame.pixels[i], g = frame.pixels[i + 1], b = frame.pixels[i + 2];
            if (!(b > r + 20 && b > g + 10)) ++ground;   // not the sky gradient: terrain
            ++total;
        }
    }
    expect(total > 0 && ground * 2 > total, "the lower half of the settled frame is terrain, not a hole");
    if (total > 0 && ground * 2 <= total) {
        std::printf("  only %lld of %lld lower-half pixels were ground\n", ground, total);
        dump_frame(frame, "terrain_stream_settled");
    }
}
