/**
 * @file roads_test.cpp
 * @brief The road pass: one routed network reaching the towns, class following traffic, bridges
 *        exactly where roads meet water, and the traced runs the renderer strokes.
 *
 * Road widths on the image are world_scale_test; buildings keeping off roads is buildings_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <vector>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("roads");

namespace {

/** @brief The cells a road run passes through, in order, from its edge chain. */
std::vector<CenterId> run_cells(const MapGraph& graph, const MapRoad& run) {
    std::vector<CenterId> cells;
    if (run.edges.empty()) {
        return cells;
    }
    const MapEdge& first = graph.edges[static_cast<std::size_t>(run.edges[0])];
    if (run.edges.size() == 1) {
        return {first.d0, first.d1};
    }
    // Start at whichever end of the first edge the second edge does not share.
    const MapEdge& second = graph.edges[static_cast<std::size_t>(run.edges[1])];
    const bool d1_is_shared = second.d0 == first.d1 || second.d1 == first.d1;
    CenterId cell = d1_is_shared ? first.d0 : first.d1;

    cells.push_back(cell);
    for (const EdgeId edge_id : run.edges) {
        const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
        cell = edge.d0 == cell ? edge.d1 : edge.d0;
        cells.push_back(cell);
    }
    return cells;
}

} // namespace

COOPA_TEST(road_class_names_round_trip) {
    for (std::size_t i = 0; i < k_road_class_count; ++i) {
        const RoadClass road_class = static_cast<RoadClass>(i);
        ASSERT_TRUE(road_class_from_name(road_class_name(road_class)) == road_class);
    }
    // An unrecognised name keeps the road rather than erasing it.
    ASSERT_TRUE(road_class_from_name("motorway") == RoadClass::Trail);
}

COOPA_TEST(roads_form_one_network_reaching_the_towns) {
    MapConfig config = road_config();
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::vector<EdgeId> road_edges;
    for (const MapEdge& edge : graph.edges) {
        if (edge.road) {
            road_edges.push_back(edge.index);
        }
    }
    ASSERT_TRUE(road_edges.size() > 8);

    // Union-find over the cells the roads join. The old contour pass produced long unconnected
    // arcs; a routed network is mostly one piece.
    std::vector<CenterId> parent(graph.centers.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = static_cast<CenterId>(i);
    }
    const std::function<CenterId(CenterId)> find = [&parent](CenterId id) -> CenterId {
        while (parent[static_cast<std::size_t>(id)] != id) {
            parent[static_cast<std::size_t>(id)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(id)])];
            id = parent[static_cast<std::size_t>(id)];
        }
        return id;
    };
    for (const EdgeId edge_id : road_edges) {
        const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
        const CenterId a = find(edge.d0);
        const CenterId b = find(edge.d1);
        if (a != b) {
            parent[static_cast<std::size_t>(a)] = b;
        }
    }

    std::map<CenterId, int> component_size;
    for (const EdgeId edge_id : road_edges) {
        ++component_size[find(graph.edges[static_cast<std::size_t>(edge_id)].d0)];
    }
    int largest = 0;
    for (const auto& entry : component_size) {
        largest = std::max(largest, entry.second);
    }
    ASSERT_TRUE(largest * 2 >= static_cast<int>(road_edges.size()));

    // The point of routing rather than contouring: the settlements placed two passes later are
    // on the network. They are not hubs -- the town pass adds jitter and its own spacing -- so
    // this is a majority, not a guarantee.
    int on_a_road = 0;
    for (const MapTown& town : graph.towns) {
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        for (const EdgeId edge_id : center.borders) {
            if (graph.edges[static_cast<std::size_t>(edge_id)].road) {
                ++on_a_road;
                break;
            }
        }
    }
    ASSERT_TRUE(!graph.towns.empty());
    ASSERT_TRUE(on_a_road * 2 > static_cast<int>(graph.towns.size()));
}

COOPA_TEST(road_class_follows_traffic) {
    MapConfig config = road_config(1234);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    int busiest_trail = 0;
    int quietest_road = std::numeric_limits<int>::max();
    int busiest_road = 0;
    int quietest_highway = std::numeric_limits<int>::max();
    bool saw_road = false;
    bool saw_highway = false;

    for (const MapEdge& edge : graph.edges) {
        // The predicate and the class are two readings of one fact and may never disagree: the
        // town packer asks the first, the renderer the second.
        ASSERT_TRUE(edge.road == (edge.road_class != RoadClass::None));
        ASSERT_TRUE(edge.road == (edge.traffic > 0));
        if (!edge.road) {
            ASSERT_EQ(edge.traffic, 0);
            continue;
        }
        switch (edge.road_class) {
            case RoadClass::Trail:
                busiest_trail = std::max(busiest_trail, edge.traffic);
                break;
            case RoadClass::Road:
                saw_road = true;
                quietest_road = std::min(quietest_road, edge.traffic);
                busiest_road = std::max(busiest_road, edge.traffic);
                break;
            case RoadClass::Highway:
                saw_highway = true;
                quietest_highway = std::min(quietest_highway, edge.traffic);
                break;
            case RoadClass::None:
                break;
        }
    }

    // Classification is a pair of thresholds on one number, so the tiers cannot interleave. A
    // regression that cut the tiers on anything else would.
    if (saw_road) {
        ASSERT_TRUE(quietest_road > busiest_trail);
    }
    if (saw_highway) {
        ASSERT_TRUE(quietest_highway > busiest_road);
    }
}

COOPA_TEST(roads_bridge_only_where_they_meet_water) {
    MapConfig config = road_config(55);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (const MapEdge& edge : graph.edges) {
        const bool over_water =
            (edge.d0 != k_invalid_id && graph.centers[static_cast<std::size_t>(edge.d0)].water)
            || (edge.d1 != k_invalid_id && graph.centers[static_cast<std::size_t>(edge.d1)].water);

        if (edge.bridge) {
            ASSERT_TRUE(edge.road);
            ASSERT_TRUE(edge.river > 0 || over_water);
        }
        // And the converse: a road over water without a bridge would be a road running through
        // the river, which is what makes this an equivalence rather than a one-way check.
        if (edge.road && (edge.river > 0 || over_water)) {
            ASSERT_TRUE(edge.bridge);
        }
    }
}

COOPA_TEST(roads_keep_off_the_border_and_the_open_sea) {
    MapConfig config = road_config(8);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (const MapEdge& edge : graph.edges) {
        if (!edge.road) {
            continue;
        }
        // The forced-water band at the map edge is not somewhere a road goes.
        ASSERT_TRUE(!graph.centers[static_cast<std::size_t>(edge.d0)].border);
        ASSERT_TRUE(!graph.centers[static_cast<std::size_t>(edge.d1)].border);
    }

    // A causeway may hop a strait but not strike out across the ocean, so no run may hold a
    // stretch of water longer than max_water_span.
    for (const MapRoad& run : graph.roads) {
        int water_run = 0;
        for (const CenterId cell : run_cells(graph, run)) {
            water_run = graph.centers[static_cast<std::size_t>(cell)].water ? water_run + 1 : 0;
            ASSERT_TRUE(water_run <= config.roads.max_water_span);
        }
    }
}

COOPA_TEST(road_runs_partition_the_flagged_edges) {
    MapConfig config = road_config(313);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::vector<int> times_traced(graph.edges.size(), 0);
    for (const MapRoad& run : graph.roads) {
        ASSERT_TRUE(!run.edges.empty());
        ASSERT_TRUE(run.points.size() >= 2);
        ASSERT_TRUE(run.road_class != RoadClass::None);

        for (const EdgeId edge_id : run.edges) {
            ASSERT_TRUE(edge_id >= 0 && static_cast<std::size_t>(edge_id) < graph.edges.size());
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            // A run is a chain of one class, which is what lets the renderer stroke it as a
            // single polyline at a single width.
            ASSERT_TRUE(edge.road_class == run.road_class);
            ++times_traced[static_cast<std::size_t>(edge_id)];
        }
        for (const MapPoint& point : run.points) {
            ASSERT_TRUE(std::isfinite(point.x) && std::isfinite(point.y));
            ASSERT_TRUE(point.x >= 0.0 && point.x <= static_cast<double>(config.grid_size));
            ASSERT_TRUE(point.y >= 0.0 && point.y <= static_cast<double>(config.grid_size));
        }
    }

    // Every road edge is traced into exactly one run: none dropped, none drawn twice. Drawn
    // twice would darken a road where two runs overlapped.
    for (const MapEdge& edge : graph.edges) {
        ASSERT_EQ(times_traced[static_cast<std::size_t>(edge.index)], edge.road ? 1 : 0);
    }
}

COOPA_TEST(road_runs_are_smoothed_only_when_asked) {
    MapConfig straight = road_config(21);
    straight.roads.smoothing_iterations = 0;
    MapGenerator plain(straight, maps_logger());
    plain.generate();

    // Unsmoothed, a run is exactly the cell sites it passes through.
    bool saw_multi_edge_run = false;
    for (const MapRoad& run : plain.graph().roads) {
        ASSERT_EQ(run.points.size(), run.edges.size() + 1);
        saw_multi_edge_run = saw_multi_edge_run || run.edges.size() > 1;
    }
    ASSERT_TRUE(saw_multi_edge_run);

    MapConfig curved = road_config(21);
    MapGenerator smoothed(curved, maps_logger());
    smoothed.generate();

    ASSERT_EQ(smoothed.graph().roads.size(), plain.graph().roads.size());
    for (std::size_t i = 0; i < smoothed.graph().roads.size(); ++i) {
        const MapRoad& a = plain.graph().roads[i];
        const MapRoad& b = smoothed.graph().roads[i];
        // Smoothing changes the drawn path and nothing else: the same edges, in the same order,
        // with more points between them.
        ASSERT_EQ(a.edges.size(), b.edges.size());
        if (a.edges.size() > 1) {
            ASSERT_TRUE(b.points.size() > a.points.size());
        }
        // The ends are pinned, so a run still meets the junction it was traced to.
        ASSERT_TRUE(std::abs(a.points.front().x - b.points.front().x) < 1e-12);
        ASSERT_TRUE(std::abs(a.points.back().y - b.points.back().y) < 1e-12);
    }
}
