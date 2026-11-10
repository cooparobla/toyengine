/**
 * @file determinism_test.cpp
 * @brief One seed, one world: the portable RNG/sort primitives and whole-map reproducibility.
 *
 * Covers the cross-platform golden sequences in portable_random.h / portable_sort.h, that two
 * generations of one config agree field for field, and that the seed actually reaches the
 * generator. Determinism of individual subsystems lives with them (organic shapes in
 * shapes_test, names in settlements_test, threaded generation in map_task_test).
 */

#include <coopa/testing/test.h>

#include <cstdint>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include <coopa/maps/portable_random.h>
#include <coopa/maps/portable_sort.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("determinism");

/**
 * @brief Pins portable_random.h / portable_sort.h to golden sequences.
 *
 * These are what make one seed produce one world on every platform: they reproduce libstdc++'s
 * distributions, shuffle and sort, which libc++ does not. The values below come from this
 * implementation, which generates the Linux-made map_out.yaml (--seed=42) byte for byte on
 * macOS, so a change here means seeded worlds have changed on at least one platform. Covers both
 * shuffle paths (paired draws for short ranges, one draw per swap past 65535) and sort's tie
 * order, below and above its 16-element insertion threshold.
 */
COOPA_TEST(portable_random_is_pinned) {
    using coopa::maps::UniformIntDistribution;
    using coopa::maps::UniformRealDistribution;
    auto fnv = [](const auto& values, auto key) {
        std::uint64_t h = 1469598103934665603ull;
        for (const auto& v : values) {
            h ^= std::uint32_t(key(v));
            h *= 1099511628211ull;
        }
        return h;
    };

    std::mt19937 rng(42);

    const int ints[] = {3, 5, 6, 2, 5, 5, 4, 4, 1, 3};
    UniformIntDistribution<int> d6(1, 6);
    for (int expected : ints) ASSERT_EQ(d6(rng), expected);

    const std::size_t sizes[] = {155, 99, 58, 459, 866, 333, 601, 142, 708, 650};
    UniformIntDistribution<std::size_t> dz(0, 999);
    for (std::size_t expected : sizes) ASSERT_EQ(dz(rng), expected);

    const double reals[] = {-2.1333247529962569, 2.1929920156962268, 3.600592643467448,
                            -2.4949380290042131, 3.9493751676478102, 1.5136297981236426};
    UniformRealDistribution<double> dr(-2.5, 4.0);
    for (double expected : reals) ASSERT_TRUE(dr(rng) == expected);

    std::vector<int> even(10);
    std::iota(even.begin(), even.end(), 0);
    coopa::maps::shuffle(even.begin(), even.end(), rng);
    ASSERT_TRUE((even == std::vector<int>{7, 2, 4, 8, 0, 3, 1, 6, 9, 5}));

    std::vector<int> odd(11);
    std::iota(odd.begin(), odd.end(), 0);
    coopa::maps::shuffle(odd.begin(), odd.end(), rng);
    ASSERT_TRUE((odd == std::vector<int>{4, 6, 1, 9, 7, 0, 3, 2, 8, 5, 10}));

    std::vector<int> large(70000);
    std::iota(large.begin(), large.end(), 0);
    coopa::maps::shuffle(large.begin(), large.end(), rng);
    ASSERT_TRUE(fnv(large, [](int v) { return v; }) == 6057529787730047087ull);

    using Keyed = std::pair<int, int>;  // (key with many ties, original position)
    auto by_key = [](const Keyed& a, const Keyed& b) { return a.first < b.first; };
    auto position = [](const Keyed& k) { return k.second; };

    std::vector<Keyed> small;
    for (int i = 0; i < 40; ++i) small.push_back({UniformIntDistribution<int>(0, 3)(rng), i});
    coopa::maps::sort(small.begin(), small.end(), by_key);
    const std::vector<int> small_order = {0, 21, 29, 31, 18, 32, 14, 35, 11, 10, 36, 3, 8, 7,
                                          13, 34, 33, 5, 4, 28, 25, 37, 38, 1, 24, 23, 22, 20,
                                          12, 6, 2, 26, 27, 30, 19, 17, 16, 15, 9, 39};
    for (std::size_t i = 0; i < small.size(); ++i) ASSERT_EQ(small[i].second, small_order[i]);

    std::vector<Keyed> big;
    for (int i = 0; i < 5000; ++i) big.push_back({UniformIntDistribution<int>(0, 9)(rng), i});
    coopa::maps::sort(big.begin(), big.end(), by_key);
    ASSERT_TRUE(fnv(big, position) == 15573018026342859319ull);
}

COOPA_TEST(generate_is_deterministic) {
    MapGenerator first(small_config(4242), maps_logger());
    MapGenerator second(small_config(4242), maps_logger());
    first.generate();
    second.generate();

    const MapGraph& a = first.graph();
    const MapGraph& b = second.graph();
    ASSERT_EQ(a.centers.size(), b.centers.size());
    ASSERT_EQ(a.corners.size(), b.corners.size());
    ASSERT_EQ(a.edges.size(), b.edges.size());
    ASSERT_EQ(a.towns.size(), b.towns.size());

    for (std::size_t i = 0; i < a.centers.size(); ++i) {
        ASSERT_TRUE(a.centers[i].biome == b.centers[i].biome);
        ASSERT_TRUE(std::abs(a.centers[i].elevation - b.centers[i].elevation) < 1e-12);
        ASSERT_TRUE(std::abs(a.centers[i].moisture - b.centers[i].moisture) < 1e-12);
    }
    // The noisy-edge pass draws random midpoints, so this is where a non-deterministic seed
    // (the wall clock, say) would show up first.
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        ASSERT_EQ(a.edges[i].noisy_points0.size(), b.edges[i].noisy_points0.size());
        ASSERT_EQ(a.edges[i].river, b.edges[i].river);
        ASSERT_EQ(a.edges[i].traffic, b.edges[i].traffic);
        ASSERT_TRUE(a.edges[i].road_class == b.edges[i].road_class);
        ASSERT_TRUE(a.edges[i].bridge == b.edges[i].bridge);
    }
    // The road pass draws no randomness, so its output is reproducible for a stronger reason
    // than a shared seed -- but it does sort candidate hubs and hub pairs, and a tie broken by
    // sort order rather than by id would show here as a network that differs between two runs
    // of the same config.
    ASSERT_EQ(a.roads.size(), b.roads.size());
    for (std::size_t i = 0; i < a.roads.size(); ++i) {
        ASSERT_TRUE(a.roads[i].road_class == b.roads[i].road_class);
        ASSERT_EQ(a.roads[i].edges.size(), b.roads[i].edges.size());
        ASSERT_EQ(a.roads[i].points.size(), b.roads[i].points.size());
        for (std::size_t j = 0; j < a.roads[i].edges.size(); ++j) {
            ASSERT_EQ(a.roads[i].edges[j], b.roads[i].edges[j]);
        }
    }
}

COOPA_TEST(different_seeds_differ) {
    MapGenerator first(small_config(1), maps_logger());
    MapGenerator second(small_config(2), maps_logger());
    first.generate();
    second.generate();

    bool any_difference = false;
    const std::size_t count =
        std::min(first.graph().centers.size(), second.graph().centers.size());
    for (std::size_t i = 0; i < count && !any_difference; ++i) {
        any_difference = first.graph().centers[i].biome != second.graph().centers[i].biome;
    }
    ASSERT_TRUE(any_difference);
}
