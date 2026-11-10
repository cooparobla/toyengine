/**
 * @file pass_rivers.h
 * @brief Fourth pass: drops river sources on the high ground and follows the
 *        downhill chain to the sea.
 */

#ifndef COOPA_MAPS_PASSES_PASS_RIVERS_H
#define COOPA_MAPS_PASSES_PASS_RIVERS_H

#include <cstddef>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/portable_random.h>

namespace coopa {
namespace maps {

/**
 * @class PassRivers
 * @brief Raises `river` volume on the corners and edges a watercourse runs through.
 *
 * Sources are sampled uniformly from the corner set and rejected unless they
 * sit in a band of middling height -- too low and the river has nowhere to
 * run, too high and it starts on a peak the elevation pass made unreachable.
 * From an accepted source the walk follows `downslope` until it reaches the
 * coast, incrementing the volume on each corner and the edge between them.
 */
class PassRivers {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassElevation` to have run.
     * @param config Supplies the seed and the number of rivers to attempt.
     * @param logger Receives progress, and a warning if the terrain rejects sources.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: rivers");

        graph.rivers.clear();
        if (graph.corners.empty() || config.river_count <= 0) {
            return;
        }

        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed));
        coopa::maps::UniformIntDistribution<std::size_t> pick(0, graph.corners.size() - 1);

        // Attempts are bounded: retrying a rejected source indefinitely would
        // spin forever on a map with no land in the accepted elevation band (an
        // all-ocean seed). A bound turns that into a logged shortfall.
        const int max_attempts = config.river_count * k_attempts_per_river;
        int placed = 0;
        for (int attempt = 0; attempt < max_attempts && placed < config.river_count; ++attempt) {
            const CornerId source = static_cast<CornerId>(pick(rng));
            const MapCorner& candidate = graph.corners[static_cast<std::size_t>(source)];
            // `water` as well as `ocean`: a lake corner is a legitimate low
            // point, and a river that starts in one is a river that starts in a
            // lake.
            if (candidate.ocean || candidate.water
                || candidate.elevation < config.river_source_min_elevation
                || candidate.elevation > config.river_source_max_elevation) {
                continue;
            }
            if (carve_river_(graph, config, source)) {
                ++placed;
            }
        }

        if (placed < config.river_count) {
            logger.warn("map pass: rivers placed " + std::to_string(placed) + " of "
                        + std::to_string(config.river_count)
                        + " requested; too little land in the source elevation band");
        }
    }

private:
    /** @brief Source draws allowed per requested river before the pass gives up. */
    static constexpr int k_attempts_per_river = 16;

    /**
     * @brief Walks one river from `source` toward the sea, keeping it only if it runs far enough.
     *
     * Two steps, and the order is the point. The walk gathers the corner chain
     * without touching the graph; only a chain of at least
     * `MapConfig::river_min_length` corners is then committed. Raising volumes as
     * it walked -- which is what this did before -- makes a short river
     * impossible to reject, because by the time you can measure it you have
     * already carved it. Most of the land is near a coast, so without the
     * rejection the map fills with two-cell trickles.
     *
     * @param graph The graph to carve into.
     * @param config Supplies the minimum length.
     * @param source The corner to start from.
     * @return True if the watercourse was long enough to keep.
     */
    bool carve_river_(MapGraph& graph, const MapConfig& config, CornerId source) const {
        std::vector<CornerId> path;
        std::vector<EdgeId> crossings;

        CornerId current = source;
        path.push_back(current);
        // The walk is strictly downhill and terminates at water or at a basin,
        // but guard the step count anyway: a downslope chain is only acyclic
        // because the elevations it was built from are, and a NaN height would
        // make that false.
        for (std::size_t step = 0; step < graph.corners.size(); ++step) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(current)];
            if (reaches_water_(corner)) {
                break;
            }
            const CornerId next = corner.downslope;
            if (next == k_invalid_id || next == current) {
                break; // Basin: nowhere lower to flow.
            }
            const EdgeId edge_id = find_edge_(graph, current, next);
            if (edge_id == k_invalid_id) {
                break;
            }
            crossings.push_back(edge_id);
            path.push_back(next);
            current = next;
        }

        if (static_cast<int>(path.size()) < config.river_min_length) {
            return false;
        }
        // A river has to end somewhere. `fill_depressions()` is what makes that
        // true -- it leaves every land corner with a strictly descending path
        // to water -- so this rejection should never fire, and
        // `execute()` logs it if it does. It stays because "always ends in a
        // water body" is a property of the output that a caller can rely on, and
        // a property enforced only by an invariant two passes away is one a
        // future change to elevation can quietly break. Enforcing it here costs
        // one comparison and cannot be got wrong.
        if (!reaches_water_(graph.corners[static_cast<std::size_t>(path.back())])) {
            return false;
        }

        for (const EdgeId edge_id : crossings) {
            graph.edges[static_cast<std::size_t>(edge_id)].river += 1;
        }
        // Every corner on the path gains volume, so a confluence -- a corner two
        // watercourses both run through -- ends up carrying both, which is what
        // makes the channel widen downstream.
        for (const CornerId corner_id : path) {
            graph.corners[static_cast<std::size_t>(corner_id)].river += 1;
        }

        MapRiver river;
        river.corners = path;
        river.points.reserve(path.size());
        for (const CornerId corner_id : path) {
            river.points.push_back(graph.corners[static_cast<std::size_t>(corner_id)].point);
        }
        river.volume = graph.corners[static_cast<std::size_t>(path.back())].river;
        // Corner-cutting rounds the joints; it does not straighten the course,
        // because it never moves a point more than a quarter of a segment. The
        // meander is the downslope chain itself and survives intact.
        chaikin_smooth(river.points, config.river_smoothing_iterations);
        for (MapPoint& point : river.points) point = graph.on_surface(point);   // a globe's cuts fall inside it
        graph.rivers.push_back(std::move(river));
        return true;
    }

    /**
     * @brief Whether a corner is in, or on the shore of, a body of water.
     *
     * `water` covers a corner with a lake or ocean cell around it; `coast` covers
     * the shoreline, which `PassCoast` deliberately excludes from `water` so the
     * two can be told apart. A river mouth is one or the other, and the union is
     * what "ends in a water body" means -- a sea mouth is a `coast` corner, a
     * lake mouth a `water` one.
     *
     * Stopping on the union rather than on `coast` alone is also what keeps a
     * river off a lake's surface. Lake corners sit below their shore, so a walk
     * that only stopped at the sea ran on down the lake bed to its lowest corner
     * and drew a channel across the water.
     */
    static bool reaches_water_(const MapCorner& corner) {
        return corner.water || corner.coast;
    }

    /**
     * @brief Finds the edge joining two adjacent corners.
     *
     * Scans the first corner's own `protrudes` list -- at most a handful of
     * entries. Scanning the whole edge array on every step of every river would
     * dominate the cost of the pass at any real grid size.
     *
     * @return The connecting edge, or `k_invalid_id` if the corners are not adjacent.
     */
    EdgeId find_edge_(const MapGraph& graph, CornerId from, CornerId to) const {
        for (const EdgeId edge_id : graph.corners[static_cast<std::size_t>(from)].protrudes) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if ((edge.v0 == from && edge.v1 == to) || (edge.v0 == to && edge.v1 == from)) {
                return edge_id;
            }
        }
        return k_invalid_id;
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_RIVERS_H
