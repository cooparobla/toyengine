/**
 * @file climate_test.cpp
 * @brief The temperature field: latitude gradient, the per-pole ice caps, and the global offset.
 *
 * The map is a north-south slice of a globe with `y = 0` at the north edge. These tests pin that
 * convention and the climate knobs; how temperature becomes a biome is biome_test.
 */

#include <coopa/testing/test.h>

#include "support/map_fixtures.h"

#include <coopa/maps/biome.h>

using namespace mapcoopa_test;

COOPA_TEST_SUITE("climate");

COOPA_TEST(temperature_follows_latitude) {
    const MapGenerator& generator = shared_small_world();
    const MapGraph& graph = generator.graph();
    const double grid_size = static_cast<double>(generator.config().grid_size);

    double polar_sum = 0.0, middle_sum = 0.0;
    std::size_t polar_count = 0, middle_count = 0;
    for (const MapCenter& center : graph.centers) {
        ASSERT_TRUE(center.temperature >= 0.0 && center.temperature <= 1.0);
        const double latitude = center.point.y / grid_size;
        if (latitude < 0.12 || latitude > 0.88) {
            polar_sum += center.temperature;
            ++polar_count;
        } else if (latitude > 0.4 && latitude < 0.6) {
            middle_sum += center.temperature;
            ++middle_count;
        }
    }
    ASSERT_TRUE(polar_count > 0 && middle_count > 0);
    // The map is a north-south slice of a globe: cold at both edges, warm through the middle.
    // Without this the biome diagram collapses back to two dimensions.
    ASSERT_TRUE(polar_sum / polar_count < middle_sum / middle_count);
}

/**
 * @brief A polar extent of zero leaves a world with no ice caps.
 *
 * The point of the knob, and the thing no value of `temperature_falloff` could ever express:
 * the exponent shapes how fast the cold arrives, never whether it arrives at all.
 *
 * Asserted on frozen *water*, which is the cleanest witness available. A lake or sea surface
 * sits at the waterline, so the altitude lapse has almost nothing to bite on, and `Ice` is
 * chosen on temperature alone (`biome.h`). Frozen ground deliberately survives an extent of
 * zero -- the lapse rate can still freeze a summit at any latitude, which is what should happen
 * to a mountain.
 */
COOPA_TEST(polar_extent_controls_the_ice) {
    const auto frozen = [](const MapGraph& graph) {
        std::size_t count = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.biome == Biome::Ice || center.biome == Biome::Glacier) {
                ++count;
            }
        }
        return count;
    };

    MapConfig none = world_config(251);
    none.polar_extent_north = 0.0;
    none.polar_extent_south = 0.0;
    MapGenerator without(none, maps_logger());
    without.generate();
    ASSERT_TRUE(frozen(without.graph()) == 0);

    MapConfig wide = world_config(251);
    wide.polar_extent_north = 0.25;
    wide.polar_extent_south = 0.25;
    MapGenerator with(wide, maps_logger());
    with.generate();
    ASSERT_TRUE(frozen(with.graph()) > 0);

    // And the caps are where they were asked for, not merely present somewhere.
    std::size_t polar = 0;
    std::size_t temperate = 0;
    const double grid = static_cast<double>(wide.grid_size);
    for (const MapCenter& center : with.graph().centers) {
        if (center.biome != Biome::Ice && center.biome != Biome::Glacier) {
            continue;
        }
        const double latitude = center.point.y / grid;
        if (latitude < 0.25 || latitude > 0.75) {
            ++polar;
        } else {
            ++temperate;
        }
    }
    ASSERT_TRUE(polar > temperate);
}

/**
 * @brief The two poles are independent: ice at one end, none at the other.
 *
 * Which hemisphere a point belongs to is decided by one comparison, and getting it backwards
 * would swap the caps without changing anything a symmetric test could see. This is that test.
 */
COOPA_TEST(polar_extents_are_independent) {
    MapConfig config = world_config(251);
    config.polar_extent_north = 0.25;
    config.polar_extent_south = 0.0;
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const double grid = static_cast<double>(config.grid_size);
    double north = 0.0;
    double south = 0.0;
    std::size_t north_count = 0;
    std::size_t south_count = 0;
    for (const MapCenter& center : generator.graph().centers) {
        const double latitude = center.point.y / grid;
        if (latitude < 0.10) {
            north += center.temperature;
            ++north_count;
        } else if (latitude > 0.90) {
            south += center.temperature;
            ++south_count;
        }
    }
    ASSERT_TRUE(north_count > 0 && south_count > 0);
    // `y = 0` is the north edge, so the cap belongs there and the far edge is left temperate.
    // Measured 0.004 against 0.253.
    ASSERT_TRUE(north / static_cast<double>(north_count) < k_biome_frigid);
    ASSERT_TRUE(south / static_cast<double>(south_count) > k_biome_frigid);
}

/**
 * @brief The global offset moves the whole world, and zero moves nothing.
 *
 * Zero has to be exact rather than approximate: it is the setting every map made before the
 * knob existed was generated at.
 */
COOPA_TEST(temperature_offset_shifts_the_world) {
    const auto mean_temperature = [](double offset) {
        MapConfig config = world_config(251);
        config.temperature_offset = offset;
        MapGenerator generator(config, maps_logger());
        generator.generate();
        double sum = 0.0;
        std::size_t count = 0;
        for (const MapCenter& center : generator.graph().centers) {
            sum += center.temperature;
            ++count;
        }
        return sum / static_cast<double>(count);
    };

    const double cold = mean_temperature(-0.25);
    const double warm = mean_temperature(0.25);

    // Zero is the untouched field, corner for corner. The shared world is world_config(251)
    // with the offset left at its default; this one spells the zero out.
    const MapGraph& reference = shared_world().graph();
    MapConfig zeroed = world_config(251);
    zeroed.temperature_offset = 0.0;
    MapGenerator zero(zeroed, maps_logger());
    zero.generate();
    ASSERT_EQ(reference.corners.size(), zero.graph().corners.size());
    for (std::size_t i = 0; i < reference.corners.size(); ++i) {
        ASSERT_TRUE(reference.corners[i].temperature == zero.graph().corners[i].temperature);
    }

    double neutral = 0.0;
    for (const MapCenter& center : zero.graph().centers) {
        neutral += center.temperature;
    }
    neutral /= static_cast<double>(zero.graph().centers.size());
    ASSERT_TRUE(cold < neutral);
    ASSERT_TRUE(neutral < warm);
}
