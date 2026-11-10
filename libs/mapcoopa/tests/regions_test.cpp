/**
 * @file regions_test.cpp
 * @brief Political geography: countries and regions partition the habitable land, and the
 *        regions layer is readable as data (every pixel one region's colour or background).
 *
 * Region names' reproducibility is checked alongside town names in settlements_test; region
 * population totals in settlements_test `population_scales_with_buildings`.
 */

#include <coopa/testing/test.h>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("regions");

COOPA_TEST(regions_partition_the_land) {
    const MapGraph& graph = shared_world().graph();

    ASSERT_TRUE(!graph.countries.empty());
    ASSERT_TRUE(!graph.regions.empty());

    for (std::size_t i = 0; i < graph.regions.size(); ++i) {
        const MapRegion& region = graph.regions[i];
        ASSERT_EQ(region.index, static_cast<RegionId>(i));
        ASSERT_TRUE(region.country >= 0
                    && region.country < static_cast<CountryId>(graph.countries.size()));
        ASSERT_TRUE(!region.name.empty());
    }

    // Every scrap of habitable land belongs to somebody. Cells the cost-weighted fill cannot
    // reach are adopted by their nearest claimant rather than left stateless.
    std::size_t unclaimed = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.water || center.ocean || center.border) {
            continue;
        }
        if (center.country == k_invalid_id) {
            ++unclaimed;
        } else {
            ASSERT_TRUE(center.country < static_cast<CountryId>(graph.countries.size()));
        }
    }
    ASSERT_EQ(unclaimed, static_cast<std::size_t>(0));
}

COOPA_TEST(regions_layer_draws_regions_and_borders) {
    MapConfig config = world_config(37);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.regions.empty());

    const BiomePalette palette;
    const Image regions = MapLayers::regions(graph, config, palette);
    ASSERT_EQ(regions.channels, 3);

    // Every pixel is exactly one region's colour or exactly the background -- nothing else, no
    // third value. That is the property that makes the layer readable as data, and it is why the
    // near-black country borders that used to be stroked over the fills are gone: they were
    // neither, and a consumer recovering a region from a pixel had no answer for them.
    std::size_t region_pixels = 0;
    for (int y = 0; y < regions.height; ++y) {
        for (int x = 0; x < regions.width; ++x) {
            const glm::vec3 found = regions.color_at(x, y);
            if (found == palette.background_color) {
                continue;
            }
            bool matched = false;
            for (const MapRegion& region : graph.regions) {
                matched = matched || found == region.color;
            }
            ASSERT_TRUE(matched);
            ++region_pixels;
        }
    }
    ASSERT_TRUE(region_pixels > 0);

    // With no political geography there is nothing to draw.
    MapConfig stateless = config;
    stateless.enable_regions = false;
    MapGenerator plain(stateless, maps_logger());
    plain.generate();
    const Image empty = MapLayers::regions(plain.graph(), stateless, palette);
    for (int y = 0; y < empty.height; ++y) {
        for (int x = 0; x < empty.width; ++x) {
            ASSERT_TRUE(empty.color_at(x, y) == palette.background_color);
        }
    }
}
