/**
 * @file pass_elevation.h
 * @brief Third pass: raises terrain by distance from the coast, reshapes the
 *        height distribution, and computes the downhill flow direction.
 */

#ifndef COOPA_MAPS_PASSES_PASS_ELEVATION_H
#define COOPA_MAPS_PASSES_PASS_ELEVATION_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>
#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/passes/drainage.h>
#include <coopa/maps/passes/water_levels.h>
#include <coopa/maps/noise.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassElevation
 * @brief Assigns `elevation` to every corner and cell, and `downslope` to every corner.
 *
 * Height is graph distance from the map border rather than a second noise
 * field, which is what makes coastlines land at sea level and mountains form
 * in the interior of a landmass rather than at random. Crossing between two
 * land corners costs a full unit; every other step costs 0.01, so water is
 * effectively free to traverse and an inland sea does not push the mountains
 * around it any higher.
 */
class PassElevation {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassCoast` to have run.
     * @param config Supplies the smoothing iterations and strength.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: elevation");

        assign_corner_elevations_(graph);
        // Between the two on purpose: the distance field decides the ordering of
        // heights and the redistribution only reshapes their histogram, so this
        // is the one place a change can move where the mountains are.
        apply_relief_(graph, config);
        redistribute_elevations_(graph, config);
        // Before downslopes, not after: the flow direction has to be derived from
        // the field rivers will actually run on, or they carve uphill.
        smooth_elevations_(graph, config);
        // Filling is what makes a downhill walk from any land corner actually
        // arrive somewhere wet; the downslopes are then read off the filled
        // field, which is why `restore_drainage()` pairs the two.
        restore_drainage(graph);
        assign_center_elevations_(graph);
        // Cells carry the mean of their corners, and a cell with few corners can
        // still sit well clear of its neighbours after the corner field is
        // smooth. Relaxing the cell field too is what the renderer and the biome
        // classifier actually read.
        smooth_center_elevations_(graph, config);
        // Last, because it reads the finished heights. It cannot live in
        // PassWater, which is where sea and lake are told apart -- that pass runs
        // first and has no elevations to level anything against yet.
        assign_water_levels(graph, config);
    }

private:
    /** @brief Cost of a step between two corners, before the land surcharge. */
    static constexpr double k_step_cost = 0.01;
    /** @brief Extra cost charged when both ends of a step are dry land. */
    static constexpr double k_land_step_cost = 1.0;
    /**
     * @brief Fraction of the distance range the relief noise fades in over.
     *
     * Narrow, so only the immediate shore is held down and the interior is free
     * to be shaped by noise. Widen it and the coastal plain grows.
     */
    static constexpr double k_coast_band = 0.12;
    /**
     * @brief Shapes the height histogram.
     *
     * Values above 1 place more of the map at low elevation, so a continent
     * reads as broad plains with a few peaks rather than a uniform ramp.
     */
    static constexpr double k_scale_factor = 1.1;

    /** @brief Breadth-first relaxation of corner heights outward from the border. */
    void assign_corner_elevations_(MapGraph& graph) const {
        std::queue<CornerId> pending;
        for (MapCorner& corner : graph.corners) {
            if (corner.border) {
                corner.elevation = 0.0;
                pending.push(corner.index);
            } else {
                corner.elevation = std::numeric_limits<double>::infinity();
            }
        }
        // A globe without a border measures height from its sea instead (water steps are all but
        // free here anyway, so on a plane the two seeds agree); failing that, from any water,
        // and failing that from one corner.
        if (pending.empty() && graph.is_sphere()) {
            for (const bool any_water : {false, true}) {
                for (MapCorner& corner : graph.corners) {
                    if (any_water ? corner.water : corner.ocean) {
                        corner.elevation = 0.0;
                        pending.push(corner.index);
                    }
                }
                if (!pending.empty()) break;
            }
            if (pending.empty() && !graph.corners.empty()) {
                graph.corners.front().elevation = 0.0;
                pending.push(graph.corners.front().index);
            }
        }

        while (!pending.empty()) {
            const CornerId current_id = pending.front();
            pending.pop();

            const double current_elevation = graph.corners[static_cast<std::size_t>(current_id)].elevation;
            const bool current_is_water = graph.corners[static_cast<std::size_t>(current_id)].water;

            for (const CornerId neighbor_id : graph.corners[static_cast<std::size_t>(current_id)].adjacent) {
                MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
                double new_elevation = k_step_cost + current_elevation;
                if (!current_is_water && !neighbor.water) {
                    new_elevation += k_land_step_cost;
                }
                if (new_elevation < neighbor.elevation) {
                    neighbor.elevation = new_elevation;
                    pending.push(neighbor.index);
                }
            }
        }
    }

    /**
     * @brief Reshapes the distance field with fractal noise, so peaks stop tracing a skeleton.
     *
     * Distance from the coast models *how high* land tends to get well, and
     * *where* badly: the maximum of a distance field is its medial axis, so on
     * its own it puts every summit on a thin ridge running equidistant between
     * the bays either side of a landmass. That reads as foam -- bright closed
     * loops around dark basins -- and not as terrain.
     *
     * So the two are **blended, not multiplied**. Scaling the distance field by
     * noise cannot fix it: distances span tens of units while a noise factor
     * spans one, so the ordering stays the distance field's and the ridge
     * survives however hard it is attenuated. Mixing toward noise replaces that
     * ordering outright, and at `relief = 1` the interior is shaped by noise
     * alone.
     *
     * Two things keep that from breaking everything downstream:
     *
     * - **A coastal mask.** The noise term is faded in over the first
     *   `k_coast_band` of the distance range, so the shore stays the lowest land
     *   there is and a coastline cannot be handed a summit.
     * Land is kept clear of the sea by the split rank remap that follows, not
     * here -- see `redistribute_elevations_()`.
     *
     * Skipped entirely at zero, so `terrain_relief = 0` is bit-for-bit the pure
     * distance field Amit Patel's original produced, offset and all.
     *
     * @param graph The graph to reshape; requires `assign_corner_elevations_()`.
     * @param config Supplies the relief amount and its noise field.
     */
    void apply_relief_(MapGraph& graph, const MapConfig& config) const {
        if (config.terrain_relief <= 0.0) {
            return;
        }
        const double relief = std::clamp(config.terrain_relief, 0.0, 1.0);

        double land_ceiling = 0.0;
        for (const MapCorner& corner : graph.corners) {
            if (corner.ocean || corner.border || !std::isfinite(corner.elevation)) {
                continue;
            }
            land_ceiling = std::max(land_ceiling, corner.elevation);
        }
        if (land_ceiling <= 0.0) {
            return;
        }

        const Noise noise(config.noise_relief);
        for (MapCorner& corner : graph.corners) {
            if (corner.ocean || corner.border || !std::isfinite(corner.elevation)) {
                continue;
            }
            const double distance = std::clamp(corner.elevation / land_ceiling, 0.0, 1.0);
            // Noise arrives in [-1, 1].
            const double unit = std::clamp(
                0.5 * (static_cast<double>(graph.noise_at(noise, corner.point)) + 1.0),
                0.0, 1.0);
            const double coast_mask = std::min(1.0, distance / k_coast_band);

            const double shaped = (1.0 - relief) * distance + relief * coast_mask * unit;
            // No shore offset any more: the split rank remap that follows puts the
            // sea below the waterline and everything else above it, so lifting
            // land clear of water here would be doing that job twice.
            corner.elevation = land_ceiling * shaped;
        }
    }

    /**
     * @brief Remaps heights by rank, into the sea below the waterline and land above it.
     *
     * Two separate remaps over one shared curve, because the sea bed and the land
     * are two different things measured in the same units. Ranking them together
     * -- which is what this did -- interleaved them: a corner far out to sea
     * accumulates enough hundredths of a step to outrank a coastal one, so the
     * sea had no consistent depth, the shoreline had no consistent height, and
     * `elevation` under water meant nothing at all. That is what made the water
     * layer render the open sea as a mottled field.
     *
     * Split, the sea occupies `[0, sea_level)` and everything else
     * `[sea_level, 1]`. Depth becomes real bathymetry, the shoreline becomes a
     * definite height, and "this ground is under water" becomes a comparison
     * worth making.
     *
     * Lakes are ranked with the *land*, not the sea. A tarn sits at altitude; the
     * only thing below sea level is the sea.
     *
     * Ranks are computed over an index array rather than by sorting the corner
     * storage: `MapGraph` guarantees `corners[i].index == i`, and every adjacency
     * list in the graph is expressed in those indices, so reordering the array
     * itself would silently corrupt the whole map.
     */
    void redistribute_elevations_(MapGraph& graph, const MapConfig& config) const {
        std::vector<CornerId> sea;
        std::vector<CornerId> ground;
        for (const MapCorner& corner : graph.corners) {
            (corner.ocean ? sea : ground).push_back(corner.index);
        }

        const double waterline = std::clamp(config.sea_level, 0.0, 1.0);
        remap_range_(graph, sea, 0.0, waterline);
        remap_range_(graph, ground, waterline, 1.0);
    }

    /**
     * @brief Rank-remaps one set of corners onto `[low, high]` through the height curve.
     *
     * The curve, `sqrt(k) - sqrt(k(1-y))`, places more of the set toward the
     * bottom of its band, so land reads as broad plains under a few peaks and the
     * sea shelves gently before it drops away.
     *
     * @param graph The graph whose corners to rewrite.
     * @param order Corner ids to rank among themselves.
     * @param low Height the lowest-ranked corner takes.
     * @param high Height the highest-ranked corner takes.
     */
    void remap_range_(MapGraph& graph, std::vector<CornerId>& order, double low,
                      double high) const {
        if (order.empty()) {
            return;
        }
        if (order.size() == 1) {
            graph.corners[static_cast<std::size_t>(order.front())].elevation = low;
            return;
        }

        coopa::maps::sort(order.begin(), order.end(), [&graph](CornerId a, CornerId b) {
            const double ea = graph.corners[static_cast<std::size_t>(a)].elevation;
            const double eb = graph.corners[static_cast<std::size_t>(b)].elevation;
            if (ea != eb) {
                return ea < eb;
            }
            return a < b; // Stable against ties, so a run is reproducible.
        });

        const double span = high - low;
        const double last = static_cast<double>(order.size() - 1);
        for (std::size_t rank = 0; rank < order.size(); ++rank) {
            const double y = static_cast<double>(rank) / last;
            double x = std::sqrt(k_scale_factor) - std::sqrt(k_scale_factor * (1.0 - y));
            x = std::clamp(x, 0.0, 1.0);
            graph.corners[static_cast<std::size_t>(order[rank])].elevation = low + span * x;
        }
    }

    /**
     * @brief Relaxes the height field toward the mean of each corner's neighbours.
     *
     * Border corners are pinned at zero so the coastline does not creep inland
     * as the interior settles. Reads from a snapshot each pass, so a corner is
     * not influenced by neighbours already moved this iteration -- doing it in
     * place would bias the result in traversal order.
     */
    void smooth_elevations_(MapGraph& graph, const MapConfig& config) const {
        if (config.elevation_smoothing_iterations <= 0 || graph.corners.empty()) {
            return;
        }
        const double strength = std::clamp(config.elevation_smoothing_strength, 0.0, 1.0);

        std::vector<double> previous(graph.corners.size());
        for (int iteration = 0; iteration < config.elevation_smoothing_iterations; ++iteration) {
            for (std::size_t i = 0; i < graph.corners.size(); ++i) {
                previous[i] = graph.corners[i].elevation;
            }
            for (MapCorner& corner : graph.corners) {
                if (corner.border || corner.adjacent.empty()) {
                    continue;
                }
                // Averaged only with neighbours on the same side of the
                // waterline. Relaxing across it drags the sea up onto the shore
                // and the shore down under it, which is how land ended up ranked
                // below water even after the split remap had separated them --
                // and the shoreline is the one edge every other pass is derived
                // from.
                double sum = 0.0;
                std::size_t counted = 0;
                for (const CornerId neighbor_id : corner.adjacent) {
                    if (graph.corners[static_cast<std::size_t>(neighbor_id)].ocean
                        != corner.ocean) {
                        continue;
                    }
                    sum += previous[static_cast<std::size_t>(neighbor_id)];
                    ++counted;
                }
                if (counted == 0) {
                    continue;
                }
                const double mean = sum / static_cast<double>(counted);
                const double own = previous[static_cast<std::size_t>(corner.index)];
                corner.elevation = own + strength * (mean - own);
            }
        }
    }

    /**
     * @brief Relaxes the per-cell height field toward each cell's neighbours.
     *
     * Water cells are left alone so the coastline keeps its step -- smoothing
     * across the shore would drag the sea up onto the land and blur the very
     * boundary the elevation pass built everything else from.
     */
    void smooth_center_elevations_(MapGraph& graph, const MapConfig& config) const {
        if (config.elevation_smoothing_iterations <= 0 || graph.centers.empty()) {
            return;
        }
        const double strength = std::clamp(config.elevation_smoothing_strength, 0.0, 1.0);

        std::vector<double> previous(graph.centers.size());
        for (int iteration = 0; iteration < config.elevation_smoothing_iterations; ++iteration) {
            for (std::size_t i = 0; i < graph.centers.size(); ++i) {
                previous[i] = graph.centers[i].elevation;
            }
            for (MapCenter& center : graph.centers) {
                if (center.water || center.border || center.neighbors.empty()) {
                    continue;
                }
                double sum = 0.0;
                std::size_t counted = 0;
                for (const CenterId neighbor_id : center.neighbors) {
                    if (graph.centers[static_cast<std::size_t>(neighbor_id)].water) {
                        continue;
                    }
                    sum += previous[static_cast<std::size_t>(neighbor_id)];
                    ++counted;
                }
                if (counted == 0) {
                    continue;
                }
                const double mean = sum / static_cast<double>(counted);
                const double own = previous[static_cast<std::size_t>(center.index)];
                center.elevation = own + strength * (mean - own);
            }
        }
    }

    /** @brief Averages corner heights down onto their cells. */
    void assign_center_elevations_(MapGraph& graph) const {
        for (MapCenter& center : graph.centers) {
            if (center.corners.empty()) {
                center.elevation = 0.0;
                continue;
            }
            double sum = 0.0;
            for (const CornerId corner_id : center.corners) {
                sum += graph.corners[static_cast<std::size_t>(corner_id)].elevation;
            }
            center.elevation = sum / static_cast<double>(center.corners.size());
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_ELEVATION_H
