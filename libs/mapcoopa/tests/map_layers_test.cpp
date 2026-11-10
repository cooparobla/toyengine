/**
 * @file map_layers_test.cpp
 * @brief The layer renderers as a set: every layer renders at the configured size with the
 *        right channels and only its own subject, the biome layer's colour matches each cell's
 *        biome, and the two composite shading modes.
 *
 * The elevation layer's surface styles are elevation_layer_test; banded/parallel rendering and
 * PNG export are export_test. Feature-specific layer behaviour lives with the feature (caves,
 * landmarks, regions, water, rivers).
 */

#include <coopa/testing/test.h>

#include <vector>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("map_layers");

COOPA_TEST(layers_separate_their_concerns) {
    MapConfig config = small_config(5);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (std::size_t i = 0; i < k_map_layer_count; ++i) {
        const MapLayer layer = static_cast<MapLayer>(i);
        const Image image = MapLayers::render(layer, graph, config);
        ASSERT_EQ(image.width, config.image_size);
        ASSERT_EQ(image.height, config.image_size);
        ASSERT_TRUE(!map_layer_name(layer).empty());

        const int expected_channels = map_layer_has_alpha(layer) ? 4 : 3;
        ASSERT_EQ(image.channels, expected_channels);
        ASSERT_EQ(image.pixels.size(),
                  static_cast<std::size_t>(image.width) * image.height * expected_channels);

        std::size_t opaque = 0;
        std::size_t transparent = 0;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.alpha_at(x, y) == 0) {
                    ++transparent;
                } else {
                    ++opaque;
                }
            }
        }
        // Anything other than a single flat colour proves the layer actually drew.
        bool varied = false;
        for (std::size_t k = static_cast<std::size_t>(expected_channels);
             k < image.pixels.size() && !varied; ++k) {
            varied = image.pixels[k] != image.pixels[k % static_cast<std::size_t>(expected_channels)];
        }
        ASSERT_TRUE(varied);

        if (map_layer_has_alpha(layer)) {
            // An overlay carries no background of its own: that is what lets it
            // stack over the terrain without hiding it.
            ASSERT_TRUE(transparent > 0);
            ASSERT_TRUE(opaque > 0);
        } else {
            ASSERT_EQ(transparent, static_cast<std::size_t>(0));
        }
    }

    // Each overlay carries only its own subject. Not a claim that they never
    // overlap in screen space -- a building beside a road legitimately does --
    // but that no layer has quietly picked up another's contents.
    const BiomePalette palette;
    const auto only_colors = [](const Image& image, const std::vector<glm::vec3>& allowed) {
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.alpha_at(x, y) == 0) {
                    continue;
                }
                const glm::vec3 found = image.color_at(x, y);
                bool matched = false;
                for (const glm::vec3& candidate : allowed) {
                    matched = matched || (found.r == candidate.r && found.g == candidate.g
                                          && found.b == candidate.b);
                }
                if (!matched) {
                    return false;
                }
            }
        }
        return true;
    };

    ASSERT_TRUE(only_colors(MapLayers::roads(graph, config),
                            {palette.trail_color, palette.road_color, palette.highway_color,
                             palette.bridge_color}));
    // The structures layer carries a settlement entire, not only its houses: the
    // square it is built around, the streets its buildings front, and the civic
    // core picked out from the dwellings. Still one layer's own subject -- none of
    // these is a road, a river or a marker.
    ASSERT_TRUE(only_colors(MapLayers::structures(graph, config),
                            {palette.building_color, palette.civic_color, palette.street_color,
                             palette.plaza_color}));
    // Cave mouths are markers too, and belong here for the same reason the others
    // do: a mouth is on the surface. The passages behind them are not, and would
    // show up as `cave_shallow_color` or `cave_deep_color` if one ever leaked in.
    ASSERT_TRUE(only_colors(MapLayers::landmarks(graph, config),
                            {palette.town_color, palette.landmark_natural_color,
                             palette.landmark_built_color, palette.cave_mouth_color}));
}

/**
 * @brief Both composite shading modes work, and they are genuinely different.
 *
 * They answer different questions, which is why both exist: elevation shading is a function of
 * height, so it says how high the ground is and the same height reads the same everywhere;
 * hillshading is a function of slope, so it sculpts the relief but cannot distinguish a slope at
 * sea level from the same slope on a summit.
 */
COOPA_TEST(composite_shading_modes) {
    for (std::size_t i = 0; i < k_composite_shading_count; ++i) {
        const CompositeShading mode = static_cast<CompositeShading>(i);
        ASSERT_TRUE(composite_shading_from_name(composite_shading_name(mode)) == mode);
    }
    ASSERT_TRUE(composite_shading_from_name("sunlight") == CompositeShading::Elevation);

    MapConfig by_height = small_config(12);
    by_height.composite_shading = CompositeShading::Elevation;
    // Stripped back to bare terrain, because the brightness comparison below
    // samples a cell at its own site and the composite draws things there. Region
    // tint would give two grassland cells in different provinces different base
    // colours; a landmark or settlement marker would cover the pixel outright,
    // and both samples would come back as marker blue.
    by_height.show_regions = false;
    by_height.enable_landmarks = false;
    by_height.enable_towns = false;
    by_height.enable_roads = false;
    MapConfig by_slope = by_height;
    by_slope.composite_shading = CompositeShading::Hillshade;

    MapGenerator generator(by_height, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Image elevation_lit = MapLayers::composite(graph, by_height);
    const Image slope_lit = MapLayers::composite(graph, by_slope);
    ASSERT_EQ(elevation_lit.pixels.size(), slope_lit.pixels.size());
    ASSERT_TRUE(elevation_lit.pixels != slope_lit.pixels);

    // Neither mode touches the height data: the elevation layer is the raw field
    // under both, which is what keeps it usable as a heightmap.
    const Image height_a = MapLayers::elevation(graph, by_height);
    const Image height_b = MapLayers::elevation(graph, by_slope);
    ASSERT_TRUE(height_a.pixels == height_b.pixels);

    // Under elevation shading, high ground really is brighter than low ground of
    // the same biome -- the property the mode exists for. Compared within one
    // biome so the palette cannot account for the difference.
    const double scale =
        static_cast<double>(by_height.image_size) / static_cast<double>(by_height.grid_size);
    const MapCenter* lowest = nullptr;
    const MapCenter* highest = nullptr;
    for (const MapCenter& center : graph.centers) {
        if (center.water || center.ocean || center.border
            || center.biome != Biome::Grassland) {
            continue;
        }
        if (!lowest || center.elevation < lowest->elevation) lowest = &center;
        if (!highest || center.elevation > highest->elevation) highest = &center;
    }
    if (lowest && highest && highest->elevation - lowest->elevation > 0.05) {
        const auto brightness_at = [&elevation_lit, scale](const MapCenter& center) {
            const glm::vec3 color = elevation_lit.color_at(
                static_cast<int>(center.point.x * scale),
                static_cast<int>(center.point.y * scale));
            return color.r + color.g + color.b;
        };
        ASSERT_TRUE(brightness_at(*highest) > brightness_at(*lowest));
    }
}

/**
 * @brief Every cell's own site pixel carries that cell's biome colour.
 *
 * The correspondence an interactive readout depends on: hover a point, resolve it to a cell, name
 * that cell's biome, and have the name match the colour on screen. `draw_biomes_()` is a flat fill
 * per cell from `biome_color_()`, so the claim should hold -- but nothing asserted it, and "the
 * layer paints the biome it says it does" is exactly what silently stops being true the day a
 * shading or blending pass is added.
 *
 * Sampled at each cell's own SITE, which is the point furthest from the trouble: the renderer
 * fills the subdivided noisy outline while a nearest-site lookup picks the straight Voronoi cell,
 * and those disagree in a band along every boundary. Sampling interiors pins the colour mapping
 * without baking that band in as if it were intended.
 *
 * `set_render_size` is not optional. `world_config` raises `grid_size` to 48 *after*
 * `small_config` sized the render for a grid of 16, leaving under three pixels per cell -- at
 * which point a truncated site pixel lands in a neighbour 12% of the time and this test measures
 * nothing but its own sampling error. At 16 px per cell that falls to a quarter of a percent.
 *
 * That last fraction is not zero and the tolerance below is deliberate. Those are cells whose site
 * sits within about a pixel of their own boundary, where `fill_triangle`'s pixel-centre coverage
 * rule awards the pixel to whichever neighbour covers it last. Measured both with and without
 * `subdivide_noisy_edges` and it does not move, so it is the rasteriser's coverage rule rather
 * than the edge wobble -- which is why the bound is a small constant and not a claim about noise.
 *
 * `show_regions` is off because it lerps every land colour 13% toward its province's hue (see
 * `biome_color_`), which would fail an exact palette comparison for reasons unrelated to biomes.
 */
COOPA_TEST(biomes_layer_paints_each_cell_its_own_biome) {
    MapConfig config = world_config(37);
    config.show_regions = false;
    set_render_size(config, 768);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.centers.empty());

    const BiomePalette palette;
    const Image biomes = MapLayers::biomes(graph, config, palette);
    ASSERT_EQ(biomes.channels, 3);

    const double scale =
        static_cast<double>(config.image_size) / static_cast<double>(config.grid_size);
    std::size_t checked = 0;
    std::size_t mismatched = 0;
    for (const MapCenter& center : graph.centers) {
        const int x = static_cast<int>(center.point.x * scale);
        const int y = static_cast<int>(center.point.y * scale);
        // The boundary ring sits outside the image; it has no pixel to check.
        if (x < 0 || y < 0 || x >= biomes.width || y >= biomes.height) continue;
        // A cell too degenerate to outline paints nothing and keeps the background.
        if (biomes.color_at(x, y) == palette.background_color) continue;

        if (!(biomes.color_at(x, y) == palette.color_for(center.biome))) ++mismatched;
        ++checked;
    }

    // Guards against the loop having skipped everything and asserted nothing.
    ASSERT_TRUE(checked > graph.centers.size() / 2);
    ASSERT_TRUE(mismatched * 100 <= checked);   // under 1%
}
