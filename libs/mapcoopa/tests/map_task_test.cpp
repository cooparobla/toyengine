/**
 * @file map_task_test.cpp
 * @brief Asynchronous generation (MapTask): threaded output equals serial output, progress is
 *        monotone to exactly 1, cancellation is reported and leaves a valid graph, and a task
 *        dropped mid-flight cancels and joins instead of writing into a dead scope.
 *
 * Lifetime and threading regressions -- all kept deliberately. Parallel *rendering* is
 * export_test.
 */

#include <coopa/testing/test.h>

#include <coopa/maps/map_task.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("map_task");

namespace {

/** @brief Element-wise comparison of two graphs; the fields a pass can write. */
void assert_graphs_match(const MapGraph& a, const MapGraph& b) {
    ASSERT_EQ(a.centers.size(), b.centers.size());
    ASSERT_EQ(a.corners.size(), b.corners.size());
    ASSERT_EQ(a.edges.size(), b.edges.size());
    ASSERT_EQ(a.roads.size(), b.roads.size());
    ASSERT_EQ(a.rivers.size(), b.rivers.size());
    ASSERT_EQ(a.towns.size(), b.towns.size());
    ASSERT_EQ(a.regions.size(), b.regions.size());
    ASSERT_EQ(a.landmarks.size(), b.landmarks.size());

    for (std::size_t i = 0; i < a.centers.size(); ++i) {
        ASSERT_TRUE(a.centers[i].biome == b.centers[i].biome);
        ASSERT_TRUE(a.centers[i].elevation == b.centers[i].elevation);
        ASSERT_TRUE(a.centers[i].moisture == b.centers[i].moisture);
        ASSERT_EQ(a.centers[i].region, b.centers[i].region);
    }
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        ASSERT_EQ(a.edges[i].river, b.edges[i].river);
        ASSERT_EQ(a.edges[i].traffic, b.edges[i].traffic);
        ASSERT_TRUE(a.edges[i].road_class == b.edges[i].road_class);
        ASSERT_EQ(a.edges[i].noisy_points0.size(), b.edges[i].noisy_points0.size());
    }
    for (std::size_t i = 0; i < a.towns.size(); ++i) {
        ASSERT_EQ(a.towns[i].center, b.towns[i].center);
        ASSERT_EQ(a.towns[i].buildings.size(), b.towns[i].buildings.size());
        ASSERT_TRUE(a.towns[i].name == b.towns[i].name);
    }
}

} // namespace

/**
 * @brief Generating on a job engine produces exactly what generating inline does.
 *
 * The guarantee the whole threading design is built around. Nothing inside
 * generation is parallel -- it is 135 ms of a 10 s run, not worth the risk -- so
 * what this really pins down is that moving the work to another thread changed
 * none of it, which is the kind of thing that silently stops being true.
 */
COOPA_TEST(async_generation_matches_serial) {
    MapConfig config = world_config(4242);

    MapGenerator serial(config, maps_logger());
    serial.generate();

    MapGenerator threaded(config, maps_logger());
    threaded.set_job_engine(&maps_engine());
    {
        MapTask task = threaded.generate_async();
        task.wait();
        ASSERT_TRUE(task.done());
        ASSERT_TRUE(!task.cancelled());
        ASSERT_TRUE(task.progress() == 1.0f);
    }
    assert_graphs_match(serial.graph(), threaded.graph());
}

/** @brief Progress runs from 0 to exactly 1 and never goes backwards. */
COOPA_TEST(task_progress_is_monotonic) {
    MapConfig config = world_config(9);
    MapGenerator generator(config, maps_logger());
    generator.set_job_engine(&maps_engine());

    MapTask task = generator.generate_async();
    float last = 0.0f;
    for (int poll = 0; poll < 100000 && !task.done(); ++poll) {
        const float now = task.progress();
        ASSERT_TRUE(now >= last);
        ASSERT_TRUE(now >= 0.0f && now <= 1.0f);
        last = now;
    }
    task.wait();
    ASSERT_TRUE(task.progress() == 1.0f);
}

/**
 * @brief A cancelled generation stops and says so, and does not corrupt anything.
 *
 * Cancellation is cooperative and checked between passes, so a task cancelled the
 * instant it is created may still have run a pass or two -- what is asserted is
 * that it reports itself cancelled and finished, not that it did nothing.
 */
COOPA_TEST(generation_can_be_cancelled) {
    MapConfig config = world_config(11);
    MapGenerator generator(config, maps_logger());
    generator.set_job_engine(&maps_engine());

    MapTask task = generator.generate_async();
    task.cancel();
    task.wait();

    ASSERT_TRUE(task.done());
    ASSERT_TRUE(task.cancelled());
    // The geometry is built before any pass runs, so it survives cancellation --
    // and every id in it still indexes its own array.
    for (const MapCenter& center : generator.graph().centers) {
        ASSERT_TRUE(center.index >= 0);
        ASSERT_TRUE(static_cast<std::size_t>(center.index) < generator.graph().centers.size());
    }
}

/** @brief A task destroyed while its work is in flight cancels and waits, not crashes. */
COOPA_TEST(task_destructor_waits) {
    MapConfig config = world_config(13);
    MapGenerator generator(config, maps_logger());
    generator.set_job_engine(&maps_engine());
    {
        MapTask task = generator.generate_async();
        // Dropped immediately, mid-flight. The destructor has to cancel and join,
        // because the job writes into `generator`'s graph and would otherwise be
        // doing so after this scope decided it was finished with it.
    }
    // Reaching here without a crash or a hang is the assertion. Generating again
    // on the same generator must then work normally.
    generator.generate();
    ASSERT_TRUE(!generator.graph().centers.empty());
}
