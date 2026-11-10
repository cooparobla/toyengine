/**
 * @file pass_roads.h
 * @brief Eighth pass: routes a road network between the places a map is worth
 *        travelling between, ranks its parts by the traffic they carry, and
 *        traces each run as a smoothed path.
 */

#ifndef COOPA_MAPS_PASSES_PASS_ROADS_H
#define COOPA_MAPS_PASSES_PASS_ROADS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassRoads
 * @brief Fills `MapEdge::road`, `road_class`, `traffic` and `bridge`, and `MapGraph::roads`.
 *
 * Roads are *routed*, not drawn. The pass builds a travel cost for every
 * Delaunay edge out of the terrain either side of it, picks the cells worth
 * connecting, and runs a least-cost path between every pair of them. An edge's
 * `traffic` is the number of those routes that chose it, and its `RoadClass`
 * falls out of that count -- a highway is a highway because everything goes
 * that way, not because anything declared it one.
 *
 * Routes are laid one at a time, longest link first, and each one sees ground
 * an earlier route already built on as `RoadConfig::reuse_discount` times as
 * dear. That single rule is what turns a fan of independent optimal paths into
 * a network: a later route bends to join an existing road rather than cutting
 * its own line a cell away, trunks consolidate, and traffic concentrates enough
 * that the hierarchy means something.
 *
 * ### Why routed rather than contoured
 *
 * Flagging every edge whose corners fall in different elevation bands traces
 * contour lines, and contour lines connect nothing: a road could run half the
 * map without passing a settlement, every road would be the same width, and
 * `TownConfig::road_bonus` would reward proximity to a contour rather than to a
 * trade route. Switchbacks still emerge here, from the cost model -- see
 * `RoadConfig::slope_cost`.
 *
 * ### Why it can run before the town pass
 *
 * Towns want roads to score sites by, and good roads want towns to connect,
 * which looks circular. It is not: this pass picks its own anchors with
 * `biome_habitability()` -- the same table `PassTowns` ranks sites with -- so it
 * depends only on biomes and rivers, and the settlements placed two passes later
 * land on the network because both passes are reading the same ground.
 *
 * The pass draws no randomness at all. Terrain decides everything, so there is
 * no stream to seed and nothing to keep in step with another pass.
 */
class PassRoads {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassElevation`, `PassRivers` and `PassBiomes`.
     * @param config Supplies the `RoadConfig` block and the town bonuses hub scoring reuses.
     * @param logger Receives the hub, route and run counts.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: roads");

        graph.roads.clear();
        for (MapEdge& edge : graph.edges) {
            edge.road = false;
            edge.road_class = RoadClass::None;
            edge.traffic = 0;
            edge.bridge = false;
        }
        if (graph.centers.empty() || config.roads.hub_count <= 0) {
            return;
        }

        const Adjacency adjacency = build_adjacency_(graph);
        std::vector<double> edge_cost = build_edge_costs_(graph, config);
        const std::vector<CenterId> hubs = choose_hubs_(graph, config);
        if (hubs.size() < 2) {
            logger.info("map pass: roads found " + std::to_string(hubs.size())
                        + " hubs, too few to connect");
            return;
        }

        const int routed = route_all_pairs_(graph, config, adjacency, edge_cost, hubs);
        classify_(graph, config, routed);
        trace_runs_(graph, config, adjacency);

        logger.info("map pass: roads linked " + std::to_string(hubs.size()) + " hubs by "
                    + std::to_string(routed) + " routes, " + std::to_string(graph.roads.size())
                    + " runs");
    }

private:
    /** @brief One step out of a cell: where it lands and which edge it crosses. */
    struct Step {
        CenterId to = k_invalid_id; /**< @brief The neighbouring cell. */
        EdgeId edge = k_invalid_id; /**< @brief The edge crossed to reach it. */
    };

    /** @brief Steps available from each cell, indexed by `CenterId`. */
    using Adjacency = std::vector<std::vector<Step>>;

    /** @brief A search state: standing in a cell, having crossed `water_run` water cells to get there. */
    struct State {
        double cost = 0.0;        /**< @brief Cost so far plus the heuristic; the queue orders on this. */
        double travelled = 0.0;   /**< @brief Cost so far, without the heuristic. */
        int index = 0;            /**< @brief Packed `(cell, water_run)` state index. */
    };

    /** @brief Orders the frontier cheapest-first; `index` breaks ties so a run is reproducible. */
    struct CheapestFirst {
        bool operator()(const State& a, const State& b) const {
            if (a.cost != b.cost) {
                return a.cost > b.cost;
            }
            return a.index > b.index;
        }
    };

    /** @brief The cost of a path that does not exist. */
    static constexpr double k_unreachable = std::numeric_limits<double>::infinity();

    /**
     * @brief Collects the steps out of every cell, from the edge list.
     *
     * Built from `MapGraph::edges` rather than read off `MapCenter::neighbors`,
     * because a route needs the edge it crossed as well as the cell it reached
     * -- that is where the river, and therefore the bridge, is recorded -- and
     * nothing guarantees `neighbors` and `borders` are in the same order.
     *
     * @param graph The map being generated.
     * @return Steps per cell, indexed by `CenterId`.
     */
    static Adjacency build_adjacency_(const MapGraph& graph) {
        Adjacency adjacency(graph.centers.size());
        for (const MapEdge& edge : graph.edges) {
            if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
                continue;
            }
            adjacency[static_cast<std::size_t>(edge.d0)].push_back({edge.d1, edge.index});
            adjacency[static_cast<std::size_t>(edge.d1)].push_back({edge.d0, edge.index});
        }
        return adjacency;
    }

    /**
     * @brief Costs every edge by the ground either side of it.
     *
     * ```
     * cost = distance * (1 + slope + height + roughness) + crossing + water
     * ```
     *
     * Every term is symmetric in the two cells, so an edge costs the same
     * travelled either way and the network does not depend on which hub of a
     * pair happened to be the source. Distance multiplies the terrain terms
     * rather than adding to them, which keeps the whole field scale-free: change
     * `grid_size` and the routes stay put.
     *
     * An edge touching a border cell is priced at infinity -- the forced-water
     * band at the map edge is not somewhere a road goes.
     *
     * @param graph The map being generated.
     * @param config Supplies the `RoadConfig` weights.
     * @return One cost per edge, indexed by `EdgeId`.
     */
    static std::vector<double> build_edge_costs_(const MapGraph& graph, const MapConfig& config) {
        const RoadConfig& roads = config.roads;
        std::vector<double> costs(graph.edges.size(), k_unreachable);

        for (const MapEdge& edge : graph.edges) {
            if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
                continue;
            }
            const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
            const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
            if (a.border || b.border) {
                continue;
            }

            const double distance = a.point.distance_to(b.point);
            const double slope = roads.slope_cost * std::abs(b.elevation - a.elevation);
            const double height = roads.elevation_cost * std::max(a.elevation, b.elevation);
            const double rough = roads.rough_ground_cost
                * (1.0 - 0.5 * (biome_habitability(a.biome) + biome_habitability(b.biome)));

            // Half the water charge on the way in and half on the way out, so a
            // cell of open water costs one crossing however a route enters it.
            const double water = roads.water_crossing_cost * 0.5
                * ((a.water ? 1.0 : 0.0) + (b.water ? 1.0 : 0.0));
            const double crossing = edge.river > 0
                ? roads.ford_cost + roads.bridge_cost_per_volume * static_cast<double>(edge.river)
                : 0.0;

            costs[static_cast<std::size_t>(edge.index)] =
                distance * (1.0 + slope + height + rough) + crossing + water;
        }
        return costs;
    }

    /**
     * @brief Picks the cells the network is routed between.
     *
     * Scored on `biome_habitability()` plus the sea and river access
     * `TownConfig` already prices, less the same penalty it puts on high
     * ground, plus the same allowance for a roomy cell. Deliberately the town
     * pass's own scoring, minus its random jitter: hubs are where settlements
     * will want to be, so the towns placed two passes later arrive on a road.
     *
     * Accepted greedily by descending score subject to `hub_min_spacing`, with
     * `CenterId` breaking ties, so the result does not depend on the sort being
     * stable.
     *
     * @param graph The map being generated.
     * @param config Supplies `RoadConfig::hub_count`, the spacing and the town bonuses.
     * @return The chosen cells, best site first.
     */
    static std::vector<CenterId> choose_hubs_(const MapGraph& graph, const MapConfig& config) {
        const TownConfig& towns = config.towns;
        const RoadConfig& roads = config.roads;

        double total_area = 0.0;
        int land_cells = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.ocean || center.border) {
                continue;
            }
            total_area += graph.cell_area(center);
            ++land_cells;
        }
        const double mean_area = land_cells > 0 ? total_area / static_cast<double>(land_cells) : 0.0;

        std::vector<CenterId> candidates;
        std::vector<double> scores(graph.centers.size(), 0.0);
        for (const MapCenter& center : graph.centers) {
            if (center.water || center.ocean || center.border) {
                continue;
            }
            double score = biome_habitability(center.biome);
            if (score <= 0.0) {
                continue;
            }
            if (center.coast) {
                score += towns.coast_bonus;
            }
            for (const EdgeId edge_id : center.borders) {
                if (graph.edges[static_cast<std::size_t>(edge_id)].river > 0) {
                    score += towns.river_bonus;
                    break;
                }
            }
            if (center.elevation > towns.elevation_penalty_start) {
                score -= (center.elevation - towns.elevation_penalty_start)
                       * towns.elevation_penalty_scale;
            }
            if (mean_area > 0.0) {
                const double relative = std::clamp(graph.cell_area(center) / mean_area, 0.0, 2.0);
                score += towns.area_bonus * (relative - 1.0);
            }
            if (score <= 0.0) {
                continue;
            }
            scores[static_cast<std::size_t>(center.index)] = score;
            candidates.push_back(center.index);
        }

        coopa::maps::sort(candidates.begin(), candidates.end(), [&scores](CenterId a, CenterId b) {
            const double sa = scores[static_cast<std::size_t>(a)];
            const double sb = scores[static_cast<std::size_t>(b)];
            if (sa != sb) {
                return sa > sb;
            }
            return a < b;
        });

        const double spacing = meters_to_grid(config, roads.hub_min_spacing_m);
        const double spacing_squared = spacing * spacing;
        std::vector<CenterId> hubs;
        for (const CenterId candidate : candidates) {
            if (static_cast<int>(hubs.size()) >= roads.hub_count) {
                break;
            }
            const MapPoint& point = graph.centers[static_cast<std::size_t>(candidate)].point;
            bool clear = true;
            for (const CenterId chosen : hubs) {
                const MapPoint& other = graph.centers[static_cast<std::size_t>(chosen)].point;
                const double dx = point.x - other.x;
                const double dy = point.y - other.y;
                const double dz = point.z - other.z;   // 0 on a plane map
                if (dx * dx + dy * dy + dz * dz < spacing_squared) {
                    clear = false;
                    break;
                }
            }
            if (clear) {
                hubs.push_back(candidate);
            }
        }
        return hubs;
    }

    /**
     * @brief Routes every pair of hubs, longest link first, reinforcing as it goes.
     *
     * Longest first because the long links are the ones that set the trunk: run
     * free-form they take the natural corridor through the terrain, and every
     * shorter link laid afterwards finds that corridor already discounted and
     * joins it rather than paralleling it. Ordering shortest-first instead
     * builds a local mesh that the long routes then have to wander through.
     *
     * A pair in different landmasses -- or separated by more open water than
     * `RoadConfig::max_water_span` allows -- simply finds no path and is skipped,
     * which is what leaves a remote island with its own self-contained network.
     *
     * @param graph The map to annotate; `MapEdge::traffic` is accumulated here.
     * @param config Supplies the routing weights.
     * @param adjacency Steps per cell.
     * @param edge_cost Per-edge travel cost, from `build_edge_costs_()`.
     * @param hubs The cells to connect.
     * @return The number of pairs that found a path.
     */
    static int route_all_pairs_(MapGraph& graph, const MapConfig& config,
                                const Adjacency& adjacency, const std::vector<double>& edge_cost,
                                const std::vector<CenterId>& hubs) {
        struct Link {
            std::size_t from = 0;      /**< @brief Index into `hubs`. */
            std::size_t to = 0;        /**< @brief Index into `hubs`. */
            double separation = 0.0;   /**< @brief Straight-line distance between the two hubs. */
        };

        std::vector<Link> links;
        links.reserve(hubs.size() * (hubs.size() - 1) / 2);
        for (std::size_t i = 0; i < hubs.size(); ++i) {
            const MapPoint& a = graph.centers[static_cast<std::size_t>(hubs[i])].point;
            for (std::size_t j = i + 1; j < hubs.size(); ++j) {
                const MapPoint& b = graph.centers[static_cast<std::size_t>(hubs[j])].point;
                links.push_back({i, j, a.distance_to(b)});
            }
        }
        coopa::maps::sort(links.begin(), links.end(), [](const Link& a, const Link& b) {
            if (a.separation != b.separation) {
                return a.separation > b.separation;
            }
            if (a.from != b.from) {
                return a.from < b.from;
            }
            return a.to < b.to;
        });

        int routed = 0;
        std::vector<EdgeId> path;
        for (const Link& link : links) {
            if (!search_(graph, config, adjacency, edge_cost, hubs[link.from], hubs[link.to],
                         path)) {
                continue;
            }
            for (const EdgeId edge_id : path) {
                ++graph.edges[static_cast<std::size_t>(edge_id)].traffic;
            }
            ++routed;
        }
        return routed;
    }

    /**
     * @brief Least-cost path between two cells, over the discounted cost field.
     *
     * A* over states of `(cell, consecutive water cells crossed)`. The water run
     * is part of the state and not a property of the cell because
     * `RoadConfig::max_water_span` bounds a *crossing*, not a location: the same
     * lake cell is reachable one hop from shore and unreachable three hops out,
     * and a plain per-cell search cannot tell those apart.
     *
     * The heuristic is the straight-line distance scaled by
     * `RoadConfig::reuse_discount`. Every edge costs at least its own length
     * times that discount, so the estimate never exceeds the true remaining cost
     * and the path A* returns is the one Dijkstra would have -- for a fraction
     * of the frontier.
     *
     * @param graph The map being searched.
     * @param config Supplies `max_water_span` and `reuse_discount`.
     * @param adjacency Steps per cell.
     * @param edge_cost Per-edge travel cost, before the reuse discount.
     * @param from The cell to start at.
     * @param to The cell to reach.
     * @param path Receives the edges crossed, in order from `from`; cleared first.
     * @return True if a path exists.
     */
    static bool search_(const MapGraph& graph, const MapConfig& config, const Adjacency& adjacency,
                        const std::vector<double>& edge_cost, CenterId from, CenterId to,
                        std::vector<EdgeId>& path) {
        path.clear();
        const RoadConfig& roads = config.roads;
        const int span_states = std::max(0, roads.max_water_span) + 1;
        const std::size_t state_count = graph.centers.size() * static_cast<std::size_t>(span_states);

        const auto pack = [span_states](CenterId cell, int run) {
            return static_cast<int>(cell) * span_states + run;
        };
        const auto cell_of = [span_states](int state) { return state / span_states; };
        const auto run_of = [span_states](int state) { return state % span_states; };

        const MapPoint& target = graph.centers[static_cast<std::size_t>(to)].point;
        const double heuristic_scale = std::clamp(roads.reuse_discount, 0.0, 1.0);
        const auto heuristic = [&graph, &target, heuristic_scale](CenterId cell) {
            return graph.centers[static_cast<std::size_t>(cell)].point.distance_to(target)
                 * heuristic_scale;
        };

        std::vector<double> best(state_count, k_unreachable);
        std::vector<int> parent(state_count, -1);
        std::vector<EdgeId> parent_edge(state_count, k_invalid_id);
        std::priority_queue<State, std::vector<State>, CheapestFirst> frontier;

        const int start = pack(from, 0);
        best[static_cast<std::size_t>(start)] = 0.0;
        frontier.push({heuristic(from), 0.0, start});

        int reached = -1;
        while (!frontier.empty()) {
            const State current = frontier.top();
            frontier.pop();
            if (current.travelled > best[static_cast<std::size_t>(current.index)]) {
                continue; // A stale entry; the state was improved after this was queued.
            }
            const CenterId cell = cell_of(current.index);
            if (cell == to) {
                reached = current.index;
                break;
            }

            for (const Step& step : adjacency[static_cast<std::size_t>(cell)]) {
                const double base = edge_cost[static_cast<std::size_t>(step.edge)];
                if (!(base < k_unreachable)) {
                    continue;
                }
                const MapCenter& next = graph.centers[static_cast<std::size_t>(step.to)];
                const int run = next.water ? run_of(current.index) + 1 : 0;
                if (run >= span_states) {
                    continue; // A longer stretch of open water than a causeway may span.
                }

                const bool already_built =
                    graph.edges[static_cast<std::size_t>(step.edge)].traffic > 0;
                const double travelled =
                    current.travelled + base * (already_built ? roads.reuse_discount : 1.0);

                const int next_state = pack(step.to, run);
                if (travelled < best[static_cast<std::size_t>(next_state)]) {
                    best[static_cast<std::size_t>(next_state)] = travelled;
                    parent[static_cast<std::size_t>(next_state)] = current.index;
                    parent_edge[static_cast<std::size_t>(next_state)] = step.edge;
                    frontier.push({travelled + heuristic(step.to), travelled, next_state});
                }
            }
        }

        if (reached < 0) {
            return false;
        }
        for (int state = reached; parent[static_cast<std::size_t>(state)] >= 0;
             state = parent[static_cast<std::size_t>(state)]) {
            path.push_back(parent_edge[static_cast<std::size_t>(state)]);
        }
        std::reverse(path.begin(), path.end());
        return true;
    }

    /**
     * @brief Turns accumulated traffic into a class, and flags the water crossings.
     *
     * `bridge` needs no geometry: a road runs along the Delaunay edge `d0`-`d1`
     * and a river along the dual Voronoi edge `v0`-`v1`, and those are the same
     * `MapEdge` -- the two cross each other by construction. So an edge that
     * carries both a road and a river *is* the crossing, and an edge whose road
     * enters a water cell is the causeway.
     *
     * @param graph The map to annotate.
     * @param config Supplies the two traffic shares.
     * @param routed How many routes were laid; the shares are taken against this.
     */
    static void classify_(MapGraph& graph, const MapConfig& config, int routed) {
        const RoadConfig& roads = config.roads;
        // Cut the tiers at a share of the routes actually laid, not at a fixed
        // count. Every pair of hubs is routed, so the traffic on even the
        // quietest spur scales with the hub count, and a fixed cutoff would mean
        // something different on every map.
        const double total = static_cast<double>(std::max(1, routed));
        const int highway_at =
            std::max(1, static_cast<int>(std::ceil(roads.highway_traffic_share * total)));
        const int road_at =
            std::max(1, static_cast<int>(std::ceil(roads.road_traffic_share * total)));

        for (MapEdge& edge : graph.edges) {
            if (edge.traffic <= 0) {
                continue;
            }
            edge.road_class = edge.traffic >= highway_at ? RoadClass::Highway
                            : edge.traffic >= road_at    ? RoadClass::Road
                                                         : RoadClass::Trail;
            edge.road = true;

            const bool over_water =
                (edge.d0 != k_invalid_id
                 && graph.centers[static_cast<std::size_t>(edge.d0)].water)
                || (edge.d1 != k_invalid_id
                    && graph.centers[static_cast<std::size_t>(edge.d1)].water);
            edge.bridge = edge.river > 0 || over_water;
        }
    }

    /**
     * @brief Traces the road edges into runs and smooths each one.
     *
     * A run is a maximal chain of same-class road edges between two junctions,
     * where a junction is any cell with other than exactly two road edges of
     * that class. Chains are started from junctions first so a spur is traced
     * from its open end rather than from somewhere in the middle; whatever is
     * left after that is a closed ring, and any of its edges will do as a start.
     *
     * Runs are seeded in ascending `EdgeId` order, so `MapGraph::roads` comes
     * out the same on every generation of the same seed.
     *
     * @param graph The map to annotate; `MapGraph::roads` is filled here.
     * @param config Supplies `RoadConfig::smoothing_iterations`.
     * @param adjacency Steps per cell.
     */
    static void trace_runs_(MapGraph& graph, const MapConfig& config, const Adjacency& adjacency) {
        const auto same_class_roads = [&graph, &adjacency](CenterId cell, RoadClass road_class) {
            std::vector<Step> steps;
            for (const Step& step : adjacency[static_cast<std::size_t>(cell)]) {
                if (graph.edges[static_cast<std::size_t>(step.edge)].road_class == road_class) {
                    steps.push_back(step);
                }
            }
            return steps;
        };

        std::vector<bool> used(graph.edges.size(), false);

        // Two sweeps: open chains first, from their end cells, then whatever
        // closed rings are left over.
        for (int sweep = 0; sweep < 2; ++sweep) {
            for (const MapEdge& seed : graph.edges) {
                const std::size_t seed_index = static_cast<std::size_t>(seed.index);
                if (seed.road_class == RoadClass::None || used[seed_index]) {
                    continue;
                }
                const RoadClass road_class = seed.road_class;

                CenterId start = seed.d0;
                if (sweep == 0) {
                    const bool d0_is_junction =
                        same_class_roads(seed.d0, road_class).size() != 2;
                    const bool d1_is_junction =
                        same_class_roads(seed.d1, road_class).size() != 2;
                    if (!d0_is_junction && !d1_is_junction) {
                        continue; // Mid-chain; a junction sweep will reach it.
                    }
                    start = d0_is_junction ? seed.d0 : seed.d1;
                }

                MapRoad run;
                run.road_class = road_class;
                run.points.push_back(graph.centers[static_cast<std::size_t>(start)].point);

                CenterId cell = start;
                EdgeId crossing = seed.index;
                while (true) {
                    const MapEdge& edge = graph.edges[static_cast<std::size_t>(crossing)];
                    used[static_cast<std::size_t>(crossing)] = true;
                    run.edges.push_back(crossing);

                    cell = edge.d0 == cell ? edge.d1 : edge.d0;
                    run.points.push_back(graph.centers[static_cast<std::size_t>(cell)].point);

                    const std::vector<Step> onward = same_class_roads(cell, road_class);
                    if (onward.size() != 2) {
                        break; // A junction, or the end of a spur.
                    }
                    const EdgeId next = onward[0].edge == crossing ? onward[1].edge
                                                                   : onward[0].edge;
                    if (used[static_cast<std::size_t>(next)]) {
                        break; // Closed the ring.
                    }
                    crossing = next;
                }

                chaikin_smooth(run.points, config.roads.smoothing_iterations);
                for (MapPoint& point : run.points) point = graph.on_surface(point);
                graph.roads.push_back(std::move(run));
            }
        }
    }

};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_ROADS_H
