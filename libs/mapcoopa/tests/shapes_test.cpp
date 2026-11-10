/**
 * @file shapes_test.cpp
 * @brief Landmass shapes: the default is exactly the old square frame, every shape confines the
 *        land, the organic shapes are seeded and stay off the canvas edge, and continent vs
 *        archipelago come out as one landmass vs several.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("shapes");

namespace {

/**
 * @brief Counts the connected groups of dry cells in a generated map.
 *
 * Land neighbouring land across a cell edge is the same landmass. Used to tell a continent from
 * an archipelago the way a reader would -- by looking at the map, not at the configuration that
 * asked for it.
 *
 * @param graph The generated graph to walk.
 * @param out_total Receives the number of dry cells found.
 * @return The size of each landmass, largest first.
 */
std::vector<std::size_t> landmass_sizes(const MapGraph& graph, std::size_t& out_total) {
    std::vector<bool> seen(graph.centers.size(), false);
    std::vector<std::size_t> sizes;
    out_total = 0;

    for (const MapCenter& start : graph.centers) {
        const std::size_t start_index = static_cast<std::size_t>(start.index);
        if (start.water || seen[start_index]) {
            continue;
        }
        std::size_t size = 0;
        std::vector<CenterId> pending{start.index};
        seen[start_index] = true;
        while (!pending.empty()) {
            const MapCenter& current = graph.centers[static_cast<std::size_t>(pending.back())];
            pending.pop_back();
            ++size;
            for (const CenterId neighbor_id : current.neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                if (!graph.centers[index].water && !seen[index]) {
                    seen[index] = true;
                    pending.push_back(neighbor_id);
                }
            }
        }
        sizes.push_back(size);
        out_total += size;
    }

    std::sort(sizes.begin(), sizes.end(), std::greater<std::size_t>());
    return sizes;
}

} // namespace

/**
 * @brief The default shape is exactly the square frame, to the last bit.
 *
 * Every default-shape map goes through `shape_inset()`, so this is the guard that matters most:
 * `min(half - |dx|)` over a canvas-spanning rectangle has to equal `min(x, grid - x, y,
 * grid - y)` for every point, or every existing map moves.
 */
COOPA_TEST(default_shape_matches_the_square_frame) {
    MapConfig config;
    config.grid_size = 40;
    const double grid = static_cast<double>(config.grid_size);

    for (double y = -3.0; y <= grid + 3.0; y += 0.37) {
        for (double x = -3.0; x <= grid + 3.0; x += 0.37) {
            const double expected = std::min(std::min(x, grid - x), std::min(y, grid - y));
            ASSERT_TRUE(std::abs(shape_inset(config, x, y) - expected) < 1e-9);
        }
    }
}

/** @brief Every shape names itself, and an unknown name falls back to a rectangle. */
COOPA_TEST(shape_names_round_trip) {
    for (std::size_t i = 0; i < k_map_shape_count; ++i) {
        const MapShape shape = static_cast<MapShape>(i);
        ASSERT_TRUE(map_shape_from_name(map_shape_name(shape)) == shape);
    }
    ASSERT_TRUE(map_shape_from_name("rect") == MapShape::Rectangle);
    ASSERT_TRUE(map_shape_from_name("continent") == MapShape::Continent);
    ASSERT_TRUE(map_shape_from_name("archipelago") == MapShape::Archipelago);
    ASSERT_TRUE(map_shape_from_name("hexagon") == MapShape::Rectangle);
}

/**
 * @brief Land lands inside the chosen shape, and the sea fills the rest.
 *
 * Checked through a generated map rather than against `shape_inset()` directly, so what is
 * verified is that the border flag really does propagate into the water pass and out the other
 * side as coastline.
 */
COOPA_TEST(shapes_confine_the_landmass) {
    struct Case { MapShape shape; double size_m; double rotation; };
    const Case cases[] = {
        {MapShape::Rectangle, 1200.0, 0.0},
        {MapShape::Circle, 1400.0, 0.0},
        {MapShape::Triangle, 1600.0, 0.5},
        {MapShape::Continent, 1400.0, 0.0},
        {MapShape::Archipelago, 700.0, 0.0},
    };

    for (const Case& test_case : cases) {
        MapConfig config = world_config(41);
        config.shape.shape = test_case.shape;
        config.shape.width_m = test_case.size_m;
        config.shape.height_m = test_case.size_m;
        config.shape.diameter_m = test_case.size_m;
        config.shape.edge_length_m = test_case.size_m;
        config.shape.continent_size_m = test_case.size_m;
        config.shape.rotation = test_case.rotation;

        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        std::size_t land = 0;
        for (const MapCenter& center : graph.centers) {
            const double inset = shape_inset(config, center.point.x, center.point.y);
            if (!center.water) {
                // Dry land is inside the shape. The cell's site can sit a little inside the
                // border band while a corner of it reaches out, which is what the tolerance
                // allows for.
                ASSERT_TRUE(inset > 0.0);
                ++land;
            }
            // Well outside the shape, everything is sea.
            if (inset < -2.0) {
                ASSERT_TRUE(center.water);
            }
        }
        ASSERT_TRUE(land > 0);
    }
}

/**
 * @brief A one-off `shape_inset()` agrees with a field held across a sweep.
 *
 * `border_check_()` builds one `ShapeField` and reuses it, while the tests and any caller
 * outside the library go through `shape_inset()`, which resolves a fresh one per call. For the
 * organic shapes that means a landmass layout redrawn from the seed every time -- if the RNG
 * stream ever depended on anything but the seed, the two paths would disagree and every
 * assertion made against `shape_inset()` would be testing a different world from the one that
 * was generated.
 */
COOPA_TEST(shape_field_matches_shape_inset) {
    for (std::size_t i = 0; i < k_map_shape_count; ++i) {
        MapConfig config = world_config(77);
        config.shape.shape = static_cast<MapShape>(i);
        config.shape.rotation = 0.4;
        const double grid = static_cast<double>(config.grid_size);

        const ShapeField field(config);
        for (double y = -2.0; y <= grid + 2.0; y += 1.7) {
            for (double x = -2.0; x <= grid + 2.0; x += 1.7) {
                ASSERT_TRUE(std::abs(field.inset(x, y) - shape_inset(config, x, y)) < 1e-12);
            }
        }
    }
}

/**
 * @brief The organic shapes leave open sea all the way round the canvas.
 *
 * Not cosmetic. The water pass marks the ocean by flooding inward from the border cells, and a
 * landmass that reaches the frame would be sliced off by it -- and worse, could wall the fill
 * out of a bay and leave the sea classified as a lake. The placement maths exists to make this
 * true for every seed, so it is checked across a spread of them rather than one.
 */
COOPA_TEST(organic_shapes_stay_off_the_canvas_edge) {
    const MapShape shapes[] = {MapShape::Continent, MapShape::Archipelago};
    for (const MapShape shape : shapes) {
        for (int seed = 1; seed <= 12; ++seed) {
            MapConfig config = world_config(seed * 131);
            config.shape.shape = shape;
            const double grid = static_cast<double>(config.grid_size);
            const ShapeField field(config);

            for (double t = 0.0; t <= grid; t += 0.5) {
                ASSERT_TRUE(field.inset(t, 0.0) < 0.0);
                ASSERT_TRUE(field.inset(t, grid) < 0.0);
                ASSERT_TRUE(field.inset(0.0, t) < 0.0);
                ASSERT_TRUE(field.inset(grid, t) < 0.0);
            }
        }
    }
}

/**
 * @brief The outline is drawn from the seed, and from nothing else.
 *
 * Two fields built from one config have to be identical or a map would not reproduce from its
 * seed; two built from different seeds have to differ, or the shape is a fixed silhouette
 * wearing a random-looking coat.
 */
COOPA_TEST(organic_shapes_are_deterministic) {
    MapConfig config = world_config(404);
    config.shape.shape = MapShape::Archipelago;
    const double grid = static_cast<double>(config.grid_size);

    const ShapeField first(config);
    const ShapeField again(config);
    MapConfig other = config;
    other.seed = 405;
    const ShapeField elsewhere(other);

    bool differs = false;
    for (double y = 0.0; y <= grid; y += 0.9) {
        for (double x = 0.0; x <= grid; x += 0.9) {
            ASSERT_TRUE(first.inset(x, y) == again.inset(x, y));
            if (std::abs(first.inset(x, y) - elsewhere.inset(x, y)) > 1e-6) {
                differs = true;
            }
        }
    }
    ASSERT_TRUE(differs);
}

/**
 * @brief A continent comes out as one landmass, not a scatter of islands.
 *
 * The island noise still carves lakes and bays out of the interior and can strand a cell or two
 * offshore, so this asks for a dominant landmass rather than a sole one: most of the dry ground
 * has to belong to a single connected mass.
 */
COOPA_TEST(continent_is_one_landmass) {
    for (int seed = 1; seed <= 5; ++seed) {
        MapConfig config = world_config(seed * 97);
        config.shape.shape = MapShape::Continent;

        MapGenerator generator(config, maps_logger());
        generator.generate();

        std::size_t total = 0;
        const std::vector<std::size_t> sizes = landmass_sizes(generator.graph(), total);
        ASSERT_TRUE(total > 0);
        ASSERT_TRUE(!sizes.empty());
        ASSERT_TRUE(static_cast<double>(sizes.front()) > 0.85 * static_cast<double>(total));
    }
}

/**
 * @brief An archipelago comes out as several landmasses, none of them the whole map.
 *
 * Deliberately loose on the count: landmasses are allowed to fuse, which is the point, so what
 * is asserted is that asking for several got several, and that no one of them swallowed the map
 * -- the failure mode when they are sized too large for the canvas to scatter them across.
 */
COOPA_TEST(archipelago_makes_several_landmasses) {
    for (int seed = 1; seed <= 5; ++seed) {
        MapConfig config = world_config(seed * 89);
        config.shape.shape = MapShape::Archipelago;
        config.shape.continent_count = 4;

        MapGenerator generator(config, maps_logger());
        generator.generate();

        std::size_t total = 0;
        const std::vector<std::size_t> sizes = landmass_sizes(generator.graph(), total);
        ASSERT_TRUE(total > 0);
        // Slivers of a cell or two are island noise, not a continent.
        std::size_t substantial = 0;
        for (const std::size_t size : sizes) {
            if (static_cast<double>(size) > 0.05 * static_cast<double>(total)) {
                ++substantial;
            }
        }
        ASSERT_TRUE(substantial >= 2);
        ASSERT_TRUE(static_cast<double>(sizes.front()) < 0.85 * static_cast<double>(total));
    }
}
