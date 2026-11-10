/**
 * @file elevation_layer_test.cpp
 * @brief The elevation layer's surface styles: interpolated is the unchanged default, flat draws
 *        one stored height per cell, blended at zero is exactly flat, blend variation is wired
 *        to its field only when asked, nothing is band-dependent, and blending keeps the rivers.
 *
 * Not tested: how smooth a blend looks (adjacent-pixel step percentiles, per-cell sharpness
 * spread) -- image-quality metrics with tuned thresholds, which this repo has no extended tier
 * for.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("elevation_layer");

/** @brief Every surface style names itself; unknown names fall back to interpolated. */
COOPA_TEST(elevation_surface_names_round_trip) {
    for (std::size_t i = 0; i < k_elevation_surface_count; ++i) {
        const ElevationSurface mode = static_cast<ElevationSurface>(i);
        ASSERT_TRUE(elevation_surface_from_name(elevation_surface_name(mode)) == mode);
    }
    ASSERT_TRUE(elevation_surface_from_name("blended") == ElevationSurface::Blended);
    ASSERT_TRUE(elevation_surface_from_name("stepped") == ElevationSurface::Interpolated);
}

/**
 * @brief The interpolated surface is the default, and is what it always was.
 *
 * The surface style is an addition, not a change: a map generated without asking
 * for one has to come out exactly as it did before the setting existed.
 */
COOPA_TEST(interpolated_surface_is_the_default) {
    MapConfig config = small_config(12);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    MapConfig spelled_out = config;
    spelled_out.elevation_surface = ElevationSurface::Interpolated;

    const Image implied = MapLayers::elevation(generator.graph(), config);
    const Image explicit_mode = MapLayers::elevation(generator.graph(), spelled_out);
    ASSERT_TRUE(implied.pixels == explicit_mode.pixels);

    MapConfig flat = config;
    flat.elevation_surface = ElevationSurface::Flat;
    const Image stepped = MapLayers::elevation(generator.graph(), flat);
    ASSERT_EQ(stepped.pixels.size(), implied.pixels.size());
    ASSERT_TRUE(stepped.pixels != implied.pixels);
}

/**
 * @brief Flat shading draws one height per cell, with a hard edge at every boundary.
 *
 * What turning the smoothing off was expected to produce and could not: those knobs
 * relax the stored heights, while the drawn surface is interpolated between them
 * regardless. This is the setting that shows the field as it is actually held --
 * `MapCenter::elevation`, one value per cell.
 *
 * Sampled at each cell's own site, which is the one point guaranteed to lie inside
 * its polygon. Probing further out has to contend with a neighbour's outline
 * bulging over the sample once the edges are subdivided, which measures the
 * rasteriser rather than the fill. Rendered larger than `small_config` for the same
 * reason -- at eight pixels to a cell the site itself rounds into a neighbour.
 *
 * Both halves are asserted. That every site matches its own stored height would
 * pass just as well on a uniform grey image, so the second half requires that
 * neighbouring cells of different stored height actually come out different *in the
 * image* -- which is what a hard edge means.
 *
 * The expectation includes the river channel, because a flat cell is its own height
 * *minus* whatever channel crosses it. On this very config one non-border site lands
 * inside a channel; it survives today only because the cut there is 0.1 m and rounds
 * to the same grey, which is luck rather than correctness and would not hold for
 * another seed.
 */
COOPA_TEST(flat_surface_draws_one_height_per_cell) {
    MapConfig config = small_config(12);
    set_render_size(config, 512);
    config.elevation_surface = ElevationSurface::Flat;
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const Image image = MapLayers::elevation(graph, config);
    // Mirrors MapLayers::pixels_per_grid_unit_, which is private to the renderer.
    const double scale = static_cast<double>(config.image_size) / config.grid_size;

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config, detail);

    const auto expected_grey = [](double elevation) {
        return static_cast<int>(static_cast<float>(std::clamp(elevation, 0.0, 1.0) * 255.0));
    };
    // The height the renderer should produce at a cell's site: its own, less any
    // channel crossing that pixel.
    const auto expected_at_site = [&](const MapCenter& center) {
        const double x = static_cast<int>(center.point.x * scale) / scale;
        const double y = static_cast<int>(center.point.y * scale) / scale;
        const double cut = graph.channel_cut(center, x, y, channels);
        return expected_grey(std::clamp(center.elevation - cut, 0.0, 1.0));
    };
    const auto grey_at_site = [&](const MapCenter& center) {
        const int x = static_cast<int>(center.point.x * scale);
        const int y = static_cast<int>(center.point.y * scale);
        if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
            return -1;
        }
        return static_cast<int>(image.color_at(x, y).r);
    };

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3 || center.border) {
            continue;
        }
        const int found = grey_at_site(center);
        if (found < 0) {
            continue;
        }
        ASSERT_TRUE(found == expected_at_site(center));
        ++checked;
    }
    ASSERT_TRUE(checked > 0);

    // The edges are hard: where two neighbours differ in stored height, the pixels
    // differ too. A fill that interpolated would blur them toward each other.
    std::size_t contrasting = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.border) {
            continue;
        }
        for (const CenterId neighbor_id : center.neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            if (neighbor.border) {
                continue;
            }
            const int here = expected_at_site(center);
            const int there = expected_at_site(neighbor);
            if (std::abs(here - there) < 4) {
                continue;
            }
            const int drawn_here = grey_at_site(center);
            const int drawn_there = grey_at_site(neighbor);
            if (drawn_here < 0 || drawn_there < 0) {
                continue;
            }
            ASSERT_TRUE(drawn_here != drawn_there);
            ++contrasting;
        }
    }
    ASSERT_TRUE(contrasting > 0);
}

/**
 * @brief Blending at zero is flat, exactly.
 *
 * The knob is a continuum between two styles that already exist, so its lower end
 * has to *be* the lower style rather than merely resemble it. It did not, at first:
 * a zero reach fell through to the interpolated return, so a blend of 0 came out
 * fully interpolated -- the opposite of what the knob says, and invisible to any
 * test that only checked the middle of the range.
 */
COOPA_TEST(blended_at_zero_is_flat) {
    MapConfig flat = small_config(12);
    set_render_size(flat, 512);
    flat.elevation_surface = ElevationSurface::Flat;

    MapConfig blended = flat;
    blended.elevation_surface = ElevationSurface::Blended;
    blended.elevation_blend = 0.0;

    MapGenerator generator(flat, maps_logger());
    generator.generate();

    const Image hard = MapLayers::elevation(generator.graph(), flat);
    const Image none = MapLayers::elevation(generator.graph(), blended);
    ASSERT_EQ(hard.pixels.size(), none.pixels.size());
    ASSERT_TRUE(hard.pixels == none.pixels);

    // And the knob does something above zero, or the equality above is vacuous.
    MapConfig some = blended;
    some.elevation_blend = 0.6;
    const Image soft = MapLayers::elevation(generator.graph(), some);
    ASSERT_TRUE(soft.pixels != hard.pixels);
}

/**
 * @brief Blend variation is wired to its noise field, and only when it is above zero.
 *
 * Zero has to mean the field is never consulted -- otherwise every map rendered before the knob
 * existed would move -- and above zero it has to be consulted, or "zero is unchanged" is
 * satisfied by a knob wired to nothing at all. How *strongly* per-cell the variation reads is a
 * tuned image-quality measurement and is not asserted here.
 */
COOPA_TEST(blend_variation_is_consulted_only_when_nonzero) {
    MapConfig base = world_config(12);
    set_render_size(base, 512);
    base.elevation_surface = ElevationSurface::Blended;
    base.elevation_blend = 0.5;
    base.noise_blend.seed = base.seed + 2;
    MapGenerator generator(base, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    MapConfig uniform = base;
    uniform.elevation_blend_variation = 0.0;
    const Image plain = MapLayers::elevation(graph, uniform);

    // Zero means the field is never consulted, and the way to say that without
    // writing a vacuous assertion is to move the field and require the image not
    // to. Comparing an explicit 0 against the *default* 0 -- which is what this
    // first checked -- compares a value with itself and passes whatever the code
    // does.
    MapConfig elsewhere = uniform;
    elsewhere.noise_blend.seed += 9999;
    elsewhere.noise_blend.frequency *= 2.0;
    ASSERT_TRUE(MapLayers::elevation(graph, elsewhere).pixels == plain.pixels);

    // And above zero it must be consulted, or the line above is satisfied by a
    // knob wired to nothing at all.
    MapConfig moved = elsewhere;
    moved.elevation_blend_variation = 0.5;
    MapConfig stayed = uniform;
    stayed.elevation_blend_variation = 0.5;
    ASSERT_TRUE(MapLayers::elevation(graph, moved).pixels
                != MapLayers::elevation(graph, stayed).pixels);
}

/**
 * @brief Varying the radius still leaves one image, whoever drew it.
 *
 * The pyramid is built inside the whole-image pass, so it is subject to the same
 * rule as the blur it generalises: run per band it would sample a field built from
 * that band's rows. Cheap to state, and the one way a spatially varying filter
 * quietly becomes band-dependent.
 */
COOPA_TEST(blend_variation_is_not_band_dependent) {
    MapConfig config = world_config(21);
    set_render_size(config, 96);
    config.elevation_surface = ElevationSurface::Blended;
    config.elevation_blend = 0.5;
    config.elevation_blend_variation = 0.8;
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const CellGeometry geometry = MapLayers::build_cell_geometry(graph, config);
    const RenderSlice whole{RowBand{}, &geometry};
    const Image reference = MapLayers::render(MapLayer::Elevation, graph, config,
                                              BiomePalette{}, whole);
    for (const int bands : {2, 5, 96}) {
        Image banded = MapLayers::allocate(MapLayer::Elevation, config, BiomePalette{});
        for (int b = 0; b < bands; ++b) {
            const int from = config.image_size * b / bands;
            const int to = config.image_size * (b + 1) / bands;
            MapLayers::render_into(banded, MapLayer::Elevation, graph, config, BiomePalette{},
                                   RenderSlice{RowBand{from, to}, &geometry});
        }
        MapLayers::finish(banded, MapLayer::Elevation, graph, config, BiomePalette{}, whole);
        ASSERT_TRUE(reference.pixels == banded.pixels);
    }
}

/**
 * @brief The blur does not wash the rivers out, because they are cut in after it.
 *
 * The reason the three stages run in the order they do. A river is a few pixels
 * across and a half-cell blur is exactly the radius that erases a feature that
 * size: blurring a raster that already carried the channels drops their contrast
 * from 15.0 grey levels to 8.0. Drawing the cells *uncut* and subtracting the
 * channel from the blurred result instead gives back all of it.
 *
 * Measured here at 6.42 grey on `flat` against 6.63 blended -- slightly better,
 * because the blur lifts the banks a shade while the cut holds the bed. The bar is
 * that blending must not cost the rivers anything against `flat`.
 */
COOPA_TEST(blended_keeps_the_rivers_crisp) {
    MapConfig flat = small_config(12);
    set_render_size(flat, 512);
    flat.elevation_surface = ElevationSurface::Flat;
    MapGenerator generator(flat, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    MapConfig blended = flat;
    blended.elevation_surface = ElevationSurface::Blended;
    blended.elevation_blend = 0.5;

    const double scale = static_cast<double>(flat.image_size) / flat.grid_size;
    const double offset = meters_to_grid(flat, flat.river_width_base_m * 2.0);

    // Mean drop from the banks to the bed, in grey levels, sampled across every
    // river of the map.
    const auto contrast = [&](const Image& image) {
        const auto grey = [&](double x, double y) {
            const int px = static_cast<int>(x * scale);
            const int py = static_cast<int>(y * scale);
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) {
                return -1;
            }
            return static_cast<int>(image.color_at(px, py).r);
        };
        double total = 0.0;
        std::size_t sampled = 0;
        for (const MapRiver& river : graph.rivers) {
            for (std::size_t i = 2; i + 2 < river.points.size(); ++i) {
                const double dx = river.points[i + 2].x - river.points[i - 2].x;
                const double dy = river.points[i + 2].y - river.points[i - 2].y;
                const double length = std::hypot(dx, dy);
                if (length < 1e-9) {
                    continue;
                }
                const double nx = -dy / length;
                const double ny = dx / length;
                const MapPoint& point = river.points[i];
                const int bed = grey(point.x, point.y);
                const int left = grey(point.x + nx * offset, point.y + ny * offset);
                const int right = grey(point.x - nx * offset, point.y - ny * offset);
                if (bed < 0 || left < 0 || right < 0) {
                    continue;
                }
                total += (left + right) * 0.5 - bed;
                ++sampled;
            }
        }
        ASSERT_TRUE(sampled > 0);
        return total / static_cast<double>(sampled);
    };

    const double hard = contrast(MapLayers::elevation(graph, flat));
    const double soft = contrast(MapLayers::elevation(graph, blended));
    ASSERT_TRUE(hard > 4.0);        // 6.42 measured; the reference is not vacuous.
    ASSERT_TRUE(soft >= hard - 0.5);  // 6.63 measured, i.e. no loss at all.
}
