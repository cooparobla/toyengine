/**
 * @file biome_test.cpp
 * @brief Biome classification: the on-disk names, the classifier's decision table, water
 *        cells always reading as water, and the habitability table's hard exclusions.
 *
 * Not tested: how many distinct biomes a given map happens to reach, or the tuned habitability
 * values themselves -- both are tuning, not contract. Temperature, which feeds the classifier,
 * is climate_test.
 */

#include <coopa/testing/test.h>

#include "support/map_fixtures.h"

#include <coopa/maps/biome.h>

using namespace mapcoopa_test;

COOPA_TEST_SUITE("biome");

COOPA_TEST(biome_name_round_trips) {
    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const Biome biome = static_cast<Biome>(i);
        ASSERT_TRUE(biome_from_name(biome_name(biome)) == biome);
    }
    // Maps saved by the original generator spelled this one wrong.
    ASSERT_TRUE(biome_from_name("temperate_decidious_forest") == Biome::TemperateDeciduousForest);
    ASSERT_TRUE(biome_from_name("not_a_biome") == Biome::Ocean);
}

COOPA_TEST(classify_biome_table) {
    // Water and shore states short-circuit the climate diagram.
    ASSERT_TRUE(classify_biome(0.9, 0.9, 0.5, false, true, false) == Biome::Ocean);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.5, false, false, true) == Biome::Beach);

    // A water cell is `Ice` or `Lake` and nothing else, at any elevation and any moisture --
    // see `water_cells_always_get_a_water_biome` for why that matters. A shallow lake must not
    // come back `Marsh`, nor a high one `Ice`.
    ASSERT_TRUE(classify_biome(0.05, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.9, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.9, true, false, false) == Biome::Lake);
    // Frozen on temperature alone, which already carries the altitude lapse rate.
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.1, true, false, false) == Biome::Ice);
    ASSERT_TRUE(classify_biome(0.05, 0.5, 0.1, true, false, false) == Biome::Ice);

    // Wetlands are *land* now: low, wet ground beside the water rather than the water itself.
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.5, false, false, false) == Biome::Marsh);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.9, false, false, false) == Biome::Swamp);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.3, false, false, false) == Biome::BorealWetland);
    // Low but dry is not a wetland, and frozen ground is permafrost not marsh.
    ASSERT_TRUE(classify_biome(0.05, 0.4, 0.5, false, false, false) != Biome::Marsh);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.1, false, false, false) != Biome::Marsh);
    // A warm wet shore is mangrove; a frozen one is tundra.
    ASSERT_TRUE(classify_biome(0.5, 0.8, 0.9, false, false, true) == Biome::Mangrove);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.1, false, false, true) == Biome::Tundra);

    // Same elevation and moisture, different latitude -- the whole point of classifying in
    // three dimensions rather than two.
    const double elevation = 0.45;
    const double moisture = 0.6;
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.1, false, false, false) == Biome::Snow);
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.3, false, false, false) == Biome::Taiga);
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.5, false, false, false)
                == Biome::TemperateDeciduousForest);
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.9, false, false, false)
                == Biome::TropicalSeasonalForest);

    // Climate extremes reach the new entries.
    ASSERT_TRUE(classify_biome(0.9, 0.3, 0.05, false, false, false) == Biome::Glacier);
    ASSERT_TRUE(classify_biome(0.2, 0.05, 0.1, false, false, false) == Biome::ColdDesert);
    ASSERT_TRUE(classify_biome(0.2, 0.3, 0.3, false, false, false) == Biome::Steppe);
    ASSERT_TRUE(classify_biome(0.2, 0.4, 0.9, false, false, false) == Biome::Savanna);
    ASSERT_TRUE(classify_biome(0.9, 0.6, 0.5, false, false, false) == Biome::AlpineMeadow);
    ASSERT_TRUE(classify_biome(0.05, 0.05, 0.9, false, false, false) == Biome::SaltFlat);
    ASSERT_TRUE(classify_biome(0.9, 0.05, 0.9, false, false, false) == Biome::VolcanicField);
}

/**
 * @brief A cell with water in it is classified as water, on every map.
 *
 * The layer a reader actually looks at is coloured by *biome*, not by `MapCenter::water`, so
 * those two disagreeing is a visible defect however sound the underlying data is. A low water
 * cell classed `Marsh` or a high one `Ice` -- dark green and near-white -- makes a river that
 * ends in a shallow lake end in what reads as forest, and a lake you cannot see is
 * indistinguishable from no lake at all.
 *
 * Asserted over a generated map rather than on the classifier alone because the two can
 * disagree through the *arguments*: `PassBiomes` passes `land_height()`, so the thresholds are
 * fractions of the land range, and that rescale can push most lakes under a land threshold such
 * as a marsh cut-off. A table test on `classify_biome()` would not see it.
 */
COOPA_TEST(water_cells_always_get_a_water_biome) {
    const MapGraph& graph = shared_world().graph();

    std::size_t water_cells = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) {
            // And the converse: dry land never claims a water biome.
            ASSERT_TRUE(center.biome != Biome::Ocean && center.biome != Biome::Lake
                        && center.biome != Biome::Ice);
            continue;
        }
        ++water_cells;
        ASSERT_TRUE(center.biome == Biome::Ocean || center.biome == Biome::Lake
                    || center.biome == Biome::Ice);
        ASSERT_TRUE(center.ocean == (center.biome == Biome::Ocean));
    }
    ASSERT_TRUE(water_cells > 0);

    // Which is what makes this true: every river empties into a cell whose *colour* is water.
    ASSERT_TRUE(!graph.rivers.empty());
    for (const MapRiver& river : graph.rivers) {
        const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
        bool into_water = false;
        for (const CenterId center_id : mouth.touches) {
            const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
            into_water = into_water
                || center.biome == Biome::Ocean || center.biome == Biome::Lake
                || center.biome == Biome::Ice;
        }
        ASSERT_TRUE(into_water);
    }
}

/**
 * @brief The habitability table's hard exclusions, and full coverage of the enum.
 *
 * The town and road passes both read this one table, so a zero here means nothing is built and
 * no road is routed cheaply through it.
 */
COOPA_TEST(biome_habitability_excludes_the_uninhabitable) {
    ASSERT_TRUE(biome_habitability(Biome::Ocean) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Lake) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Ice) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Glacier) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Scorched) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::VolcanicField) == 0.0);

    // Every value is in range, and the switch covers the whole enum -- a biome added without a
    // case would fall through to the 0.0 return and be silently uninhabitable.
    std::size_t habitable = 0;
    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const double value = biome_habitability(static_cast<Biome>(i));
        ASSERT_TRUE(value >= 0.0 && value <= 1.0);
        if (value > 0.0) ++habitable;
    }
    ASSERT_TRUE(habitable > 0);
}
