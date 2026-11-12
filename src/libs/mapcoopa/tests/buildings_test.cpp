/**
 * @file buildings_test.cpp
 * @brief Building footprints: inside their own cells, never overlapping, clear of roadways,
 *        the market square and rivers, and sized within the configured range.
 *
 * Every check is on the *rotated* footprint, because a building tested by its centre or its
 * axis-aligned box can still lean over a boundary with a corner. Settlement-level structure
 * (tiers, streets, civic roles) is settlements_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("buildings");

COOPA_TEST(buildings_lie_inside_their_cell) {
    const MapGraph& graph = shared_small_world().graph();

    std::size_t total_buildings = 0;
    for (const MapTown& town : graph.towns) {
        // A settlement covers every cell in `cells`, not just its primary one, so a building has
        // to fall inside *one of* them -- but still wholly inside that one, never straddling a
        // boundary.
        std::vector<std::vector<MapPoint>> polygons;
        for (const CenterId cell_id : town.cells) {
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            std::vector<MapPoint> polygon;
            for (const CornerId corner_id : cell.corners) {
                polygon.push_back(graph.corners[static_cast<std::size_t>(corner_id)].point);
            }
            polygons.push_back(std::move(polygon));
        }
        ASSERT_TRUE(!polygons.empty());

        for (const MapBuilding& building : town.buildings) {
            ++total_buildings;
            ASSERT_TRUE(building.width > 0.0 && building.height > 0.0);
            // The *rotated* corners, not an axis-aligned box. Checking the box would pass even
            // when a building drawn at its stated yaw hangs out over the cell boundary.
            const std::array<MapPoint, 4> corners = building_corners(building);
            bool contained = false;
            for (const std::vector<MapPoint>& polygon : polygons) {
                bool all_in = true;
                for (const MapPoint& corner : corners) {
                    all_in = all_in && point_in_polygon(polygon, corner);
                }
                if (all_in) {
                    contained = true;
                    break;
                }
            }
            ASSERT_TRUE(contained);
        }
    }
    ASSERT_TRUE(total_buildings > 0);
}

COOPA_TEST(buildings_do_not_overlap) {
    std::size_t compared = 0;
    for (const MapTown& town : shared_small_world().graph().towns) {
        for (std::size_t i = 0; i < town.buildings.size(); ++i) {
            for (std::size_t j = i + 1; j < town.buildings.size(); ++j) {
                ASSERT_TRUE(!buildings_overlap(town.buildings[i], town.buildings[j]));
                ++compared;
            }
        }
    }
    ASSERT_TRUE(compared > 0);
}

/**
 * @brief Nothing is built on a street or a road.
 *
 * The test this needed and did not have. `can_place_()` kept buildings inside their cell, clear
 * of water and clear of each other -- and said nothing at all about the roadways, so **37% of
 * buildings stood on a lane and 18% on a road** while every layout test passed.
 *
 * Tested against the *whole footprint* rather than its corners, because that was the second half
 * of the same bug: a corner test misses a street crossing the middle of a large plot, where all
 * four corners are further from the centreline than the clearance, and misses a short road
 * segment lying wholly inside one.
 */
COOPA_TEST(nothing_is_built_on_a_roadway) {
    const MapGenerator& generator = shared_world();
    const MapConfig& config = generator.config();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.towns.empty());

    // The drawn corridor as a rotated box, which is what the packer tests against.
    const auto corridor = [](const MapPoint& from, const MapPoint& to, double half_width) {
        MapBuilding box;
        box.point = {(from.x + to.x) * 0.5, (from.y + to.y) * 0.5};
        box.width = std::max(from.distance_to(to), 1e-9);
        box.height = std::max(half_width * 2.0, 1e-9);
        box.rotation = std::atan2(to.y - from.y, to.x - from.x);
        return box;
    };

    const double lane = meters_to_grid(config, config.towns.street_width_m) * 0.5;
    std::size_t checked = 0;
    for (const MapTown& town : graph.towns) {
        for (const MapBuilding& building : town.buildings) {
            for (const MapStreet& street : town.streets) {
                ASSERT_TRUE(!buildings_overlap(corridor(street.from, street.to, lane), building));
            }
            for (const MapRoad& road : graph.roads) {
                const double half = road_width_for(config, road.road_class) * 0.5;
                for (std::size_t i = 0; i + 1 < road.points.size(); ++i) {
                    ASSERT_TRUE(!buildings_overlap(
                        corridor(road.points[i], road.points[i + 1], half), building));
                }
            }
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief Nothing is built in the market square.
 *
 * The square is open ground or it is not a square. Tested against the *rotated* footprint,
 * because a building tested by its centre can still lean into the plaza with a corner.
 */
COOPA_TEST(nothing_is_built_in_the_square) {
    const MapGraph& graph = shared_world().graph();

    std::size_t with_plaza = 0;
    for (const MapTown& town : graph.towns) {
        if (town.plaza.radius <= 0.0) {
            continue;
        }
        ++with_plaza;
        for (const MapBuilding& building : town.buildings) {
            for (const MapPoint& corner : building_corners(building)) {
                ASSERT_TRUE(town.plaza.centre.distance_to(corner) >= town.plaza.radius);
            }
        }
    }
    // And squares exist at all -- an assertion over an empty set proves nothing.
    ASSERT_TRUE(with_plaza > 0);
}

COOPA_TEST(buildings_avoid_rivers) {
    const MapGenerator& generator = shared_world();
    const MapGraph& graph = generator.graph();
    const MapConfig& config = generator.config();

    std::size_t checked = 0;
    for (const MapTown& town : graph.towns) {
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.river <= 0 || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }
            const MapPoint& a = graph.corners[static_cast<std::size_t>(edge.v0)].point;
            const MapPoint& b = graph.corners[static_cast<std::size_t>(edge.v1)].point;
            const double clearance = river_width(config, edge.river) * 0.5
                                   + meters_to_grid(config, config.towns.water_clearance_m);

            for (const MapBuilding& building : town.buildings) {
                for (const MapPoint& corner : building_corners(building)) {
                    // Distance from the footprint corner to the river's centreline.
                    const double dx = b.x - a.x;
                    const double dy = b.y - a.y;
                    const double length_squared = dx * dx + dy * dy;
                    double t = length_squared == 0.0
                        ? 0.0
                        : ((corner.x - a.x) * dx + (corner.y - a.y) * dy) / length_squared;
                    t = std::clamp(t, 0.0, 1.0);
                    const double distance =
                        std::hypot(corner.x - (a.x + t * dx), corner.y - (a.y + t * dy));
                    ASSERT_TRUE(distance >= clearance - 1e-9);
                    ++checked;
                }
            }
        }
    }
    ASSERT_TRUE(checked > 0);
}

COOPA_TEST(building_sizes_span_the_range) {
    const MapGenerator& generator = shared_world();
    const MapConfig& config = generator.config();

    // Footprints are stored in grid units and configured in metres, so everything here is
    // compared in grid units. (The monolithic suite seeded the running min/max with the metre
    // values, which made the spread check below vacuous.)
    const double min_size = meters_to_grid(config, config.towns.building_size_min_m);
    const double max_size = meters_to_grid(config, config.towns.building_size_max_m);
    double smallest = max_size;
    double largest = min_size;
    std::size_t count = 0;
    for (const MapTown& town : generator.graph().towns) {
        for (const MapBuilding& building : town.buildings) {
            ASSERT_TRUE(building.width >= min_size - 1e-9);
            ASSERT_TRUE(building.width <= max_size + 1e-9);
            ASSERT_TRUE(std::abs(building.width - building.height) < 1e-9);
            smallest = std::min(smallest, building.width);
            largest = std::max(largest, building.width);
            ++count;
        }
    }
    ASSERT_TRUE(count > 0);
    // Actually varied, not one size repeated -- the range has to be used.
    ASSERT_TRUE(largest - smallest > (max_size - min_size) * 0.5);
}
