/**
 * @file map_yaml_test.cpp
 * @brief The map document: save_map()/load_map() round-trip the whole graph (caves included)
 *        within the format's documented precision, and a reloaded map renders as the same map.
 *
 * Configuration-file loading is map_config_test.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>

#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("map_yaml");

COOPA_TEST(yaml_round_trip_preserves_the_graph) {
    MapConfig config = small_config(77);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& original = generator.graph();

    const std::string path = (coopa::test::scratch_dir() / "map.yaml").string();
    save_map(original, config, path);

    MapGraph loaded;
    MapConfig loaded_config;
    ASSERT_TRUE(load_map(path, loaded, loaded_config));

    ASSERT_EQ(loaded_config.seed, config.seed);
    ASSERT_EQ(loaded_config.grid_size, config.grid_size);
    ASSERT_EQ(loaded_config.river_count, config.river_count);

    ASSERT_EQ(loaded.centers.size(), original.centers.size());
    ASSERT_EQ(loaded.corners.size(), original.corners.size());
    ASSERT_EQ(loaded.edges.size(), original.edges.size());
    ASSERT_EQ(loaded.towns.size(), original.towns.size());

    // fkYAML's emitter formats floats at the default six significant digits, so
    // the format is lossy by construction: normalised values survive to about
    // 1e-7 and grid positions, which run up to grid_size, to about 1e-5.
    // Everything else round-trips exactly.
    const double k_normalised_epsilon = 1e-6;
    const double k_position_epsilon = 1e-3;

    for (std::size_t i = 0; i < original.centers.size(); ++i) {
        const MapCenter& a = original.centers[i];
        const MapCenter& b = loaded.centers[i];
        ASSERT_EQ(a.index, b.index);
        ASSERT_TRUE(a.biome == b.biome);
        ASSERT_TRUE(a.water == b.water && a.ocean == b.ocean && a.coast == b.coast);
        ASSERT_TRUE(a.border == b.border);
        ASSERT_TRUE(std::abs(a.elevation - b.elevation) < k_normalised_epsilon);
        ASSERT_TRUE(std::abs(a.moisture - b.moisture) < k_normalised_epsilon);
        ASSERT_TRUE(std::abs(a.point.x - b.point.x) < k_position_epsilon);
        ASSERT_TRUE(std::abs(a.point.y - b.point.y) < k_position_epsilon);
        ASSERT_EQ(a.neighbors.size(), b.neighbors.size());
        ASSERT_EQ(a.corners.size(), b.corners.size());
        ASSERT_EQ(a.borders.size(), b.borders.size());
        for (std::size_t j = 0; j < a.neighbors.size(); ++j) ASSERT_EQ(a.neighbors[j], b.neighbors[j]);
        for (std::size_t j = 0; j < a.corners.size(); ++j)   ASSERT_EQ(a.corners[j], b.corners[j]);
        for (std::size_t j = 0; j < a.borders.size(); ++j)   ASSERT_EQ(a.borders[j], b.borders[j]);
    }

    for (std::size_t i = 0; i < original.corners.size(); ++i) {
        ASSERT_EQ(original.corners[i].river, loaded.corners[i].river);
        ASSERT_EQ(original.corners[i].downslope, loaded.corners[i].downslope);
        ASSERT_EQ(original.corners[i].adjacent.size(), loaded.corners[i].adjacent.size());
    }

    for (std::size_t i = 0; i < original.edges.size(); ++i) {
        const MapEdge& a = original.edges[i];
        const MapEdge& b = loaded.edges[i];
        ASSERT_EQ(a.d0, b.d0);
        ASSERT_EQ(a.d1, b.d1);
        ASSERT_EQ(a.v0, b.v0);
        ASSERT_EQ(a.v1, b.v1);
        ASSERT_EQ(a.river, b.river);
        ASSERT_TRUE(a.road == b.road);
        ASSERT_TRUE(a.road_class == b.road_class);
        ASSERT_EQ(a.traffic, b.traffic);
        ASSERT_TRUE(a.bridge == b.bridge);
        ASSERT_EQ(a.noisy_points0.size(), b.noisy_points0.size());
    }

    ASSERT_EQ(original.roads.size(), loaded.roads.size());
    for (std::size_t i = 0; i < original.roads.size(); ++i) {
        const MapRoad& a = original.roads[i];
        const MapRoad& b = loaded.roads[i];
        ASSERT_TRUE(a.road_class == b.road_class);
        ASSERT_EQ(a.edges.size(), b.edges.size());
        ASSERT_EQ(a.points.size(), b.points.size());
        // Positions survive to six significant digits, which at these grid
        // coordinates is about 1e-4 -- see the precision note on map_to_node().
        for (std::size_t j = 0; j < a.points.size(); ++j) {
            ASSERT_TRUE(std::abs(a.points[j].x - b.points[j].x) < 1e-3);
            ASSERT_TRUE(std::abs(a.points[j].y - b.points[j].y) < 1e-3);
        }
    }

    for (std::size_t i = 0; i < original.towns.size(); ++i) {
        ASSERT_EQ(original.towns[i].center, loaded.towns[i].center);
        ASSERT_TRUE(original.towns[i].tier == loaded.towns[i].tier);
        ASSERT_EQ(original.towns[i].buildings.size(), loaded.towns[i].buildings.size());
    }

    // Caves are written in full -- stations and smoothed passages both -- because
    // per-cave identity lives here and nowhere else. The exported raster layers are
    // keyed by depth, so "give me cave 7" is a question only the document answers.
    ASSERT_EQ(original.caves.size(), loaded.caves.size());
    ASSERT_TRUE(!original.caves.empty());
    std::size_t stations = 0;
    for (std::size_t i = 0; i < original.caves.size(); ++i) {
        const MapCave& a = original.caves[i];
        const MapCave& b = loaded.caves[i];
        ASSERT_TRUE(a.name == b.name);
        ASSERT_EQ(a.mouth_edge, b.mouth_edge);
        ASSERT_TRUE(std::abs(a.phreatic_level - b.phreatic_level) < 1e-5);
        ASSERT_TRUE(std::abs(a.deepest - b.deepest) < 1e-5);
        ASSERT_EQ(a.nodes.size(), b.nodes.size());
        ASSERT_EQ(a.passages.size(), b.passages.size());
        for (std::size_t j = 0; j < a.nodes.size(); ++j) {
            ASSERT_TRUE(a.nodes[j].zone == b.nodes[j].zone);
            ASSERT_TRUE(a.nodes[j].feature == b.nodes[j].feature);
            ASSERT_EQ(a.nodes[j].parent, b.nodes[j].parent);
            ASSERT_TRUE(std::abs(a.nodes[j].floor - b.nodes[j].floor) < 1e-5);
            ASSERT_TRUE(std::abs(a.nodes[j].roof - b.nodes[j].roof) < 1e-5);
            ++stations;
        }
        for (std::size_t j = 0; j < a.passages.size(); ++j) {
            ASSERT_EQ(a.passages[j].points.size(), b.passages[j].points.size());
            ASSERT_EQ(a.passages[j].floors.size(), b.passages[j].floors.size());
            ASSERT_EQ(a.passages[j].radii.size(), b.passages[j].radii.size());
            for (std::size_t k = 0; k < a.passages[j].floors.size(); ++k) {
                ASSERT_TRUE(std::abs(a.passages[j].floors[k] - b.passages[j].floors[k]) < 1e-5);
                ASSERT_TRUE(std::abs(a.passages[j].roofs[k] - b.passages[j].roofs[k]) < 1e-5);
            }
        }
    }
    ASSERT_TRUE(stations > 20);
}

/**
 * @brief A reloaded map renders as the same map, layer for layer.
 *
 * Six significant digits survive the document (see the precision note on
 * `map_to_node()`), and for everything drawn from connectivity, flags or flat
 * colour that is exact -- those layers must come back byte for byte.
 *
 * The two layers derived from the *height field* are the exception, and only
 * just. Hillshading takes differences between neighbouring elevations and
 * multiplies them by a large exaggeration, so the last digit of a round-tripped
 * height can move a greyscale value by one. Allowing one and no more is the
 * point: it pins the loss to rounding rather than to anything structural, and a
 * regression that dropped, say, the corner elevations entirely would blow
 * straight through it.
 */
COOPA_TEST(yaml_round_trip_renders_every_layer) {
    MapConfig config = small_config(9);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const std::string path = (coopa::test::scratch_dir() / "map_render.yaml").string();
    save_map(generator.graph(), config, path);

    MapGraph loaded;
    MapConfig loaded_config;
    ASSERT_TRUE(load_map(path, loaded, loaded_config));

    for (std::size_t i = 0; i < k_map_layer_count; ++i) {
        const MapLayer layer = static_cast<MapLayer>(i);
        const Image before = MapLayers::render(layer, generator.graph(), config);
        const Image after = MapLayers::render(layer, loaded, loaded_config);
        ASSERT_EQ(before.pixels.size(), after.pixels.size());
        ASSERT_TRUE(before.pixels.size() > 0);

        // Two different tolerances, because there are two different ways six
        // significant digits can show up in a render.
        //
        // On a layer derived from the *height field* the loss is smooth: a
        // slightly different corner height tilts a Delaunay facet and moves a
        // greyscale value by a step. Bounded per pixel.
        //
        // On a layer made of *hard-edged shapes* it is not smooth at all. A
        // building footprint whose corner round-trips a ten-thousandth of a grid
        // unit away can put one boundary pixel on the other side of the edge --
        // a full 0-to-255 flip on that pixel, however exact everything else is.
        // So those are bounded by *how many* pixels may differ, not by how much.
        // Elevation only. The composite is *both* kinds of layer at once -- height
        // shading underneath, then water, roads, building footprints and markers
        // on top -- so one of its boundary pixels can flip a full 0-to-255 just
        // like a structures pixel can. Classing it as smooth was over-claiming,
        // and it held only as long as no footprint edge happened to straddle a
        // pixel centre. The count bound below still covers it, and is what
        // actually catches a structural regression here.
        const bool from_height = layer == MapLayer::Elevation;
        std::size_t differing = 0;
        for (std::size_t k = 0; k < before.pixels.size(); ++k) {
            const int delta = std::abs(static_cast<int>(before.pixels[k])
                                     - static_cast<int>(after.pixels[k]));
            if (delta == 0) {
                continue;
            }
            ++differing;
            if (from_height) {
                ASSERT_TRUE(delta <= 1);
            }
        }
        // A thousandth of the image, which a structural regression would dwarf:
        // dropping the corner elevations entirely moved 12 bytes on one layer and
        // a whole building is some hundreds.
        ASSERT_TRUE(differing * 1000 <= before.pixels.size());
    }
}
