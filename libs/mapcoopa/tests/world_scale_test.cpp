/**
 * @file world_scale_test.cpp
 * @brief The metre <-> grid <-> pixel arithmetic, the edge grade built on it, and the rasteriser
 *        drawing a feature configured in metres at that many pixels in every direction.
 *
 * These three numbers -- grid size, metres per cell, metres per pixel -- are what make every
 * other size in the config mean something, so an error here silently rescales the entire world
 * rather than breaking anything visibly. Loading the scale from a file is map_config_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("world_scale");

COOPA_TEST(world_scale_arithmetic) {
    MapConfig config;
    config.grid_size = 80;
    config.meters_per_grid_unit = 60.0;
    config.meters_per_pixel = 1.0;

    // 80 cells x 60 m = 4.8 km, at one pixel to the metre.
    ASSERT_EQ(derive_image_size(config), 4800);
    ASSERT_TRUE(std::abs(meters_to_grid(config, 60.0) - 1.0) < 1e-12);
    ASSERT_TRUE(std::abs(grid_to_meters(config, 1.0) - 60.0) < 1e-12);
    ASSERT_TRUE(std::abs(meters_to_grid(config, grid_to_meters(config, 0.37)) - 0.37) < 1e-12);

    // Coarser pixels, same world.
    config.meters_per_pixel = 4.0;
    ASSERT_EQ(derive_image_size(config), 1200);

    // What --image-size does: back-compute the scale so the two cannot disagree.
    const double world_meters = static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    config.meters_per_pixel = world_meters / 2400.0;
    ASSERT_EQ(derive_image_size(config), 2400);
    ASSERT_TRUE(std::abs(config.meters_per_pixel - 2.0) < 1e-12);
}

/** @brief `edge_grade()` is a real grade: rise over run, in metres, both ways. */
COOPA_TEST(edge_grade_is_a_real_grade) {
    MapConfig config = world_config(5);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    bool measured = false;
    for (const MapEdge& edge : graph.edges) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
            continue;
        }
        const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
        const double rise = height_to_meters(config, std::abs(a.elevation - b.elevation));
        const double run = grid_to_meters(config, a.point.distance_to(b.point));
        if (run <= 0.0) {
            continue;
        }
        ASSERT_TRUE(std::abs(edge_grade(graph, edge, config) - rise / run) < 1e-9);
        measured = true;
    }
    ASSERT_TRUE(measured);

    // Doubling the vertical scale doubles every grade, and doubling the horizontal scale halves
    // it. That is what makes a threshold written against this a statement about terrain rather
    // than about units.
    MapConfig taller = config;
    taller.elevation_range_m *= 2.0;
    MapConfig wider = config;
    wider.meters_per_grid_unit *= 2.0;
    const MapEdge& sample = graph.edges[graph.edges.size() / 2];
    if (sample.d0 != k_invalid_id && sample.d1 != k_invalid_id) {
        const double base = edge_grade(graph, sample, config);
        ASSERT_TRUE(std::abs(edge_grade(graph, sample, taller) - base * 2.0) < 1e-9);
        ASSERT_TRUE(std::abs(edge_grade(graph, sample, wider) - base * 0.5) < 1e-9);
    }
}

/**
 * @brief A feature configured in metres is drawn that many pixels across.
 *
 * The whole point of the scale: at one pixel to the metre a width read off a render is a
 * measurement. Checked against `draw_line()` directly rather than against a generated map,
 * because a road in a map is curved and a scanline across a curve measures the secant, not the
 * width.
 */
COOPA_TEST(features_render_at_their_configured_size) {
    MapConfig config;
    config.grid_size = 80;
    config.meters_per_grid_unit = 60.0;
    config.meters_per_pixel = 1.0;
    config.image_size = derive_image_size(config);
    const double scale = static_cast<double>(config.image_size) / config.grid_size;

    // Mirrors MapLayers::half_width_pixels_, which is private to the renderer.
    const auto half_width_for = [scale](double width_grid) { return width_grid * scale * 0.5; };
    const auto drawn_width = [](double half_width) {
        Image image;
        image.reset(200, 200, 3, glm::vec3(0.0f));
        draw_line(image, 20.0, 100.0, 180.0, 100.0, half_width, glm::vec3(255.0f));
        int best = 0, run = 0;
        for (int y = 0; y < image.height; ++y) {
            run = image.color_at(100, y).r > 0.0f ? run + 1 : 0;
            best = std::max(best, run);
        }
        return best;
    };

    // One pixel of slack either way: an even width cannot be centred on a pixel row, so an
    // axis-aligned stroke -- which is what this measures -- rounds up to an odd row count.
    for (const RoadClass road_class : {RoadClass::Trail, RoadClass::Road, RoadClass::Highway}) {
        const int meters = static_cast<int>(road_width_meters(config, road_class));
        const int pixels = drawn_width(half_width_for(road_width_for(config, road_class)));
        ASSERT_TRUE(std::abs(pixels - meters) <= 1);
    }
    for (const int volume : {0, 3, 10}) {
        const int meters = static_cast<int>(river_width_meters(config, volume));
        const int pixels = drawn_width(half_width_for(river_width(config, volume)));
        ASSERT_TRUE(std::abs(pixels - meters) <= 1);
    }

    // Building footprints are stored in grid units but configured in metres.
    ASSERT_TRUE(std::abs(grid_to_meters(config, meters_to_grid(config, config.towns.building_size_min_m))
                         - config.towns.building_size_min_m) < 1e-9);
}

/**
 * @brief A stroke is the width it was asked for, whichever way it runs.
 *
 * Two brush-based strokes get this wrong, and neither shows in a horizontal measurement. A
 * **square** brush widens a line by up to sqrt(2) off the axes, so a diagonal 6 m road draws
 * 8 m wide. A round brush stamped along an 8-connected path makes the opposite error -- the path
 * advances sqrt(2) of ground per step, so a diagonal draws 0.707 of its width. `draw_line()`
 * paints by distance to the segment and has neither problem.
 *
 * Measured as painted area over Euclidean length, which is direction-independent; a scanline
 * measures the secant across anything not perpendicular to it. The tolerance is a pixel and a
 * bit: pixel centres sit on integers, so an axis-aligned band of even width has to round to an
 * odd row count, and that parity is irreducible however the stroke is defined.
 */
COOPA_TEST(stroke_width_is_direction_independent) {
    const auto mean_width = [](double x0, double y0, double x1, double y1, double width) {
        Image image;
        image.reset(400, 400, 3, glm::vec3(0.0f));
        draw_line(image, x0, y0, x1, y1, width * 0.5, glm::vec3(255.0f));
        std::size_t painted = 0;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.color_at(x, y).r > 0.0f) {
                    ++painted;
                }
            }
        }
        return static_cast<double>(painted) / std::hypot(x1 - x0, y1 - y0);
    };

    for (const double width : {3.0, 5.0, 6.0, 10.0, 25.0}) {
        const double flat = mean_width(40, 200, 360, 200, width);
        const double diagonal = mean_width(40, 40, 360, 360, width);

        // Both orientations land within rasterisation parity of the width they were given. A
        // square brush put the diagonal 40% over; a brush stamped along an 8-connected path put
        // it 30% under. Either would blow through this at every width tested.
        ASSERT_TRUE(std::abs(flat - width) <= 1.6);
        ASSERT_TRUE(std::abs(diagonal - width) <= 1.6);
    }
}
