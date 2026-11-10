/**
 * @file presets_test.cpp
 * @brief The standard map and the derived values: `standard_config()` is exactly what a bare
 *        `mapcoopa` run generates, and `derived_config()` makes the seed reach every field.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>

#include <root_directory.h>

#include <coopa/maps/map_presets.h>
#include <coopa/maps/map_yaml.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("presets");

/**
 * @brief The shipped config file adds nothing to `standard_config()`.
 *
 * The CLI layers `standard_config()` under `assets/config.yaml`. A host embedding the library has
 * no config file, so the two must describe the same map, or "the default world" would mean one
 * thing in the CLI and another everywhere else.
 */
COOPA_TEST(standard_config_is_the_cli_default) {
    MapConfig layered = standard_config();
    const ConfigLoadResult loaded = load_config(std::string(ROOT_DIR) + "/assets/config.yaml", layered);
    ASSERT_TRUE(loaded.status == ConfigLoad::Ok);
    ASSERT_EQ(fkyaml::node::serialize(config_to_node(layered)),
              fkyaml::node::serialize(config_to_node(standard_config())));
}

COOPA_TEST(derived_config_follows_the_seed_once) {
    MapConfig authored = standard_config();
    authored.seed = 7;
    const MapConfig derived = derived_config(authored);
    ASSERT_EQ(derived.noise_island.seed, 7);
    ASSERT_EQ(derived.noise_temperature.seed, 8);
    ASSERT_EQ(derived.noise_blend.seed, 9);
    ASSERT_EQ(derived.noise_cave.seed, 10);
    ASSERT_TRUE(std::abs(derived.noise_island.frequency - authored.noise_island.frequency * 40.0 / 80.0) < 1e-12);
    // From the authored config again: the same answer, never scaled twice.
    ASSERT_TRUE(derived_config(authored).noise_island.frequency == derived.noise_island.frequency);
}

/**
 * @brief Changing the seed moves the coastline.
 *
 * Without `derived_config()` the land/water field keeps its own fixed seed, so a host that set
 * only `seed` got the same coastline every time -- only the rivers and regions moved.
 */
COOPA_TEST(the_seed_moves_the_coastline) {
    const auto water_of = [](int seed) {
        MapConfig authored = small_config(seed);
        authored.seed = seed;
        MapGenerator generator(derived_config(authored), maps_logger());
        generator.generate();
        std::vector<bool> water;
        for (const MapCenter& center : generator.graph().centers) water.push_back(center.water);
        return water;
    };
    const std::vector<bool> a = water_of(1);
    const std::vector<bool> b = water_of(2);
    ASSERT_EQ(a.size(), b.size());
    std::size_t differ = 0;
    for (std::size_t i = 0; i < a.size(); ++i) differ += a[i] != b[i];
    ASSERT_TRUE(differ * 20 > a.size());  // over 5% of cells change sides
}
