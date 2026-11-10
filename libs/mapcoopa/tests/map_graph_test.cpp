/**
 * @file map_graph_test.cpp
 * @brief The MapGraph's structural contract: id identity, index ranges, finite outlines, and
 *        the pass toggles leaving the geometry alone.
 *
 * Every other suite indexes the graph through these ids, so a broken invariant here shows up
 * everywhere as an out-of-range read. What each pass writes *into* the graph is tested in that
 * pass's own suite.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <vector>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("map_graph");

COOPA_TEST(graph_invariants_hold) {
    const MapGraph& graph = shared_small_world().graph();

    ASSERT_TRUE(!graph.centers.empty());
    ASSERT_TRUE(!graph.corners.empty());
    ASSERT_TRUE(!graph.edges.empty());

    const auto center_count = static_cast<CenterId>(graph.centers.size());
    const auto corner_count = static_cast<CornerId>(graph.corners.size());
    const auto edge_count = static_cast<EdgeId>(graph.edges.size());

    for (std::size_t i = 0; i < graph.centers.size(); ++i) {
        const MapCenter& center = graph.centers[i];
        // A pass that sorted this array in place (the moisture pass ranks by wetness) would
        // break the identity between a slot and its own index.
        ASSERT_EQ(center.index, static_cast<CenterId>(i));
        ASSERT_TRUE(center.elevation >= 0.0 && center.elevation <= 1.0);
        ASSERT_TRUE(center.moisture >= 0.0 && center.moisture <= 1.0);
        for (const CenterId id : center.neighbors) ASSERT_TRUE(id >= 0 && id < center_count);
        for (const CornerId id : center.corners)   ASSERT_TRUE(id >= 0 && id < corner_count);
        for (const EdgeId id : center.borders)     ASSERT_TRUE(id >= 0 && id < edge_count);
    }

    for (std::size_t i = 0; i < graph.corners.size(); ++i) {
        const MapCorner& corner = graph.corners[i];
        ASSERT_EQ(corner.index, static_cast<CornerId>(i));
        ASSERT_TRUE(corner.elevation >= 0.0 && corner.elevation <= 1.0);
        ASSERT_TRUE(corner.moisture >= 0.0 && corner.moisture <= 1.0);
        ASSERT_TRUE(corner.downslope >= 0 && corner.downslope < corner_count);
        for (const CenterId id : corner.touches)   ASSERT_TRUE(id >= 0 && id < center_count);
        for (const EdgeId id : corner.protrudes)   ASSERT_TRUE(id >= 0 && id < edge_count);
        for (const CornerId id : corner.adjacent)  ASSERT_TRUE(id >= 0 && id < corner_count);
    }

    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const MapEdge& edge = graph.edges[i];
        ASSERT_EQ(edge.index, static_cast<EdgeId>(i));
        ASSERT_TRUE(edge.d0 >= 0 && edge.d0 < center_count);
        ASSERT_TRUE(edge.d1 >= 0 && edge.d1 < center_count);
        ASSERT_TRUE(edge.v0 >= 0 && edge.v0 < corner_count);
        ASSERT_TRUE(edge.v1 >= 0 && edge.v1 < corner_count);
        ASSERT_TRUE(edge.d0 != edge.d1);
    }
}

/** @brief Every drawable cell's outline is a real polygon with no NaN in it. */
COOPA_TEST(cell_outline_is_closed_and_finite) {
    const MapGraph& graph = shared_small_world().graph();

    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3) {
            continue; // Boundary-ring cells outside the map proper.
        }
        const std::vector<MapPoint> outline = graph.cell_outline(center);
        ASSERT_TRUE(outline.size() >= 3);
        for (const MapPoint& point : outline) {
            ASSERT_TRUE(std::isfinite(point.x) && std::isfinite(point.y));
        }
    }
}

COOPA_TEST(noisy_edges_subdivide_only_when_enabled) {
    MapConfig on = small_config();
    on.subdivide_noisy_edges = true;
    MapGenerator subdivided(on, maps_logger());
    subdivided.generate();

    std::size_t wobbled = 0;
    for (const MapEdge& edge : subdivided.graph().edges) {
        ASSERT_TRUE(edge.noisy_points0.size() >= 2);
        if (edge.noisy_points0.size() > 2) ++wobbled;
    }
    ASSERT_TRUE(wobbled > 0);

    MapConfig off = small_config();
    off.subdivide_noisy_edges = false;
    MapGenerator straight(off, maps_logger());
    straight.generate();

    for (const MapEdge& edge : straight.graph().edges) {
        ASSERT_EQ(edge.noisy_points0.size(), static_cast<std::size_t>(2));
        ASSERT_EQ(edge.noisy_points1.size(), static_cast<std::size_t>(2));
    }
}

COOPA_TEST(disabled_passes_leave_the_graph_untouched) {
    MapConfig config = small_config();
    config.enable_rivers = false;
    config.enable_roads = false;
    config.enable_towns = false;

    MapGenerator generator(config, maps_logger());
    generator.generate();

    ASSERT_TRUE(generator.graph().towns.empty());
    ASSERT_TRUE(generator.graph().roads.empty());
    for (const MapEdge& edge : generator.graph().edges) {
        ASSERT_EQ(edge.river, 0);
        ASSERT_TRUE(!edge.road);
        ASSERT_TRUE(edge.road_class == RoadClass::None);
        ASSERT_EQ(edge.traffic, 0);
        ASSERT_TRUE(!edge.bridge);
    }
    // Geometry is built before any pass runs, so it survives them all being off.
    ASSERT_TRUE(!generator.graph().centers.empty());
}
