/**
 * @file pass_valleys.h
 * @brief Sixth pass: cuts the valleys the rivers run in into the height field.
 */

#ifndef COOPA_MAPS_PASSES_PASS_VALLEYS_H
#define COOPA_MAPS_PASSES_PASS_VALLEYS_H

#include <algorithm>
#include <cstddef>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/passes/drainage.h>
#include <coopa/maps/passes/water_levels.h>

namespace coopa {
namespace maps {

/**
 * @class PassValleys
 * @brief Lowers the ground along every watercourse, so rivers sit in valleys.
 *
 * Rivers erode, and this pass is what tells the height field so. Elevation is
 * computed before rivers are routed -- it has to be, the routing follows
 * `MapCorner::downslope` -- so without it the drainage network would be drawn
 * across a surface with nowhere for the water to go: the elevation layer would
 * show no trace of the rivers the water layer is full of, and a mesh built from
 * the data would have rivers running over flat ground.
 *
 * ### A valley, not a channel
 *
 * `MapGraph::elevation_at()` interpolates *cell-site* heights over Delaunay
 * triangles, while rivers run along Voronoi edges -- cell *boundaries*. So
 * lowering corner heights alone would change nothing anyone can see: the cells
 * have to move, and the narrowest feature they can express is about a cell
 * across, 60 m at the default scale, against a river 5 to 20 m wide.
 *
 * That is the right answer regardless. A river sits in a valley far wider than
 * itself, and it is the valley that belongs in a height field -- the channel
 * within it is what `river_width_base_m` draws on the water layer.
 *
 * ### Why it does not simply paint
 *
 * Nothing here paints: dimming rivers into the elevation layer would make it a
 * picture of the terrain rather than the terrain itself, and a consumer flooding
 * a mesh to those values would find channels already cut. The ground is
 * genuinely lower, the water surface `stroke_river_surface_()` draws sits
 * `river_depth_m` above the bed it finds at the bottom of the valley, and
 * flooding a mesh to that surface gives a consistent answer.
 */
class PassValleys {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to carve; requires `PassRivers` to have run.
     * @param config Supplies the incision depth, valley width and waterline.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: valleys");

        // Zero depth has to leave the field bit-identical, not merely close: it
        // is the setting that reproduces every map made before valleys existed,
        // and the only way to be sure of that is to touch nothing at all.
        if (config.river_incision_m <= 0.0 && config.river_incision_per_volume_m <= 0.0) {
            return;
        }
        if (graph.rivers.empty()) {
            return;
        }

        std::vector<double> carve = measure_incision_(graph, config);
        open_valley_(graph, config, carve);
        lower_corners_(graph, config, carve);
        enforce_downhill_(graph);
        // Moving the ground owes the map its drainage back. Carving digs pits
        // wherever a valley wall grades down into ground with no outlet, and the
        // clamp at the waterline flattens river mouths into ties -- both leave
        // corners with nowhere lower to go. `downslope` is stale for the same
        // reason: it was read off the field as the elevation pass left it, and
        // that field is not this one.
        restore_drainage(graph);
        lower_centers_(graph, config, carve);
        // A valley cut through a lake's shore lowers the rim the lake was levelled against, so
        // the levels are settled again on the carved field -- or the lake would stand over the
        // gap the river just opened.
        assign_water_levels(graph, config);
    }

private:
    /**
     * @brief How many corners a river takes to open from a gully into a valley.
     *
     * A headwater is a steep notch, not a valley floor -- the erosion that widens
     * one takes a discharge the first corner of a river does not carry. Without
     * the taper a river begins with its full valley already cut, which reads as a
     * trench starting out of nothing halfway up a hillside.
     *
     * It shapes the head of a river; it does not *protect* it. A source lying
     * within `river_valley_width` of a larger river is still carved by that
     * river's valley, which is correct -- the ground there really is lower.
     */
    static constexpr double k_valley_taper_corners = 3.0;

    /**
     * @brief Depth to cut at every corner a river runs through, in height units.
     *
     * `max` rather than `+` where two rivers share a corner: a confluence is one
     * valley, not two stacked, and volume already accounts for the extra water.
     *
     * @param graph The graph to measure; reads `rivers` and `MapCorner::river`.
     * @param config Supplies the incision depths and the vertical scale.
     * @return One depth per corner, zero away from any watercourse.
     */
    std::vector<double> measure_incision_(const MapGraph& graph, const MapConfig& config) const {
        std::vector<double> carve(graph.corners.size(), 0.0);

        for (const MapRiver& river : graph.rivers) {
            for (std::size_t i = 0; i < river.corners.size(); ++i) {
                const std::size_t index = static_cast<std::size_t>(river.corners[i]);
                const double volume = static_cast<double>(graph.corners[index].river);
                const double meters = config.river_incision_m
                                    + config.river_incision_per_volume_m * volume;
                const double taper =
                    std::min(1.0, static_cast<double>(i) / k_valley_taper_corners);
                carve[index] = std::max(carve[index], meters_to_height(config, meters) * taper);
            }
        }
        return carve;
    }

    /**
     * @brief Grades the incision outward so the valley has walls rather than sides.
     *
     * `river_valley_width` rings over `MapCorner::adjacent`, each carved
     * `river_valley_falloff` as deeply as the one inside it. Reading from a copy
     * per ring is what keeps it a fixed-radius spread instead of a flood fill --
     * carried in place, one deep corner would walk its depth across the whole map.
     *
     * @param graph The graph whose corner adjacency to walk.
     * @param config Supplies the valley width and falloff.
     * @param carve The depth field to widen, in place.
     */
    void open_valley_(const MapGraph& graph, const MapConfig& config,
                      std::vector<double>& carve) const {
        const double falloff = std::clamp(config.river_valley_falloff, 0.0, 1.0);
        if (falloff <= 0.0) {
            return;
        }

        for (int ring = 0; ring < config.river_valley_width; ++ring) {
            std::vector<double> widened = carve;
            for (const MapCorner& corner : graph.corners) {
                const double depth = carve[static_cast<std::size_t>(corner.index)] * falloff;
                if (depth <= 0.0) {
                    continue;
                }
                for (const CornerId neighbor_id : corner.adjacent) {
                    const std::size_t index = static_cast<std::size_t>(neighbor_id);
                    widened[index] = std::max(widened[index], depth);
                }
            }
            carve = std::move(widened);
        }
    }

    /**
     * @brief Cuts the measured depth out of the corner heights.
     *
     * Water corners are skipped. A lake bed is not a valley; the lake levels are
     * re-settled on the carved field once this pass is done.
     *
     * Land stops at the waterline. Carving a river mouth through it would put dry
     * ground below sea level, which is the one thing every downstream pass reads
     * the height field to decide.
     *
     * @param graph The graph to lower.
     * @param config Supplies the waterline.
     * @param carve The depth field.
     */
    void lower_corners_(MapGraph& graph, const MapConfig& config,
                        const std::vector<double>& carve) const {
        for (MapCorner& corner : graph.corners) {
            if (corner.water) {
                continue;
            }
            const double depth = carve[static_cast<std::size_t>(corner.index)];
            corner.elevation = std::max(config.sea_level, corner.elevation - depth);
        }
    }

    /**
     * @brief Restores the descent along every watercourse.
     *
     * The widening does not respect which corner is upstream of which, so where a
     * larger river passes close by, one of its rings can land on an upstream
     * corner and cut it below its own downstream neighbour. Water would then be
     * running uphill in the middle of a river. One clamping walk per river fixes
     * it, and costs nothing next to the spread that caused it.
     *
     * @param graph The graph to fix up.
     */
    void enforce_downhill_(MapGraph& graph) const {
        for (const MapRiver& river : graph.rivers) {
            for (std::size_t i = 1; i < river.corners.size(); ++i) {
                MapCorner& upstream =
                    graph.corners[static_cast<std::size_t>(river.corners[i - 1])];
                MapCorner& corner = graph.corners[static_cast<std::size_t>(river.corners[i])];
                corner.elevation = std::min(corner.elevation, upstream.elevation);
            }
        }
    }

    /**
     * @brief Dips the cells with their corners, which is what makes the valley visible.
     *
     * Subtracts the *mean* of the cell's corner depths rather than recomputing the
     * height from the carved corners: `smooth_center_elevations_()` has already
     * moved cells off the corner mean, and that relaxation is what keeps the
     * terrain from reading as facets. Taking the mean of the depths applies the
     * valley on top of it instead of throwing it away, and grades the cells
     * flanking a watercourse by how much of their outline it runs along.
     *
     * @param graph The graph to lower.
     * @param config Supplies the waterline.
     * @param carve The depth field over corners.
     */
    void lower_centers_(MapGraph& graph, const MapConfig& config,
                        const std::vector<double>& carve) const {
        for (MapCenter& center : graph.centers) {
            if (center.water || center.corners.empty()) {
                continue;
            }
            double sum = 0.0;
            for (const CornerId corner_id : center.corners) {
                sum += carve[static_cast<std::size_t>(corner_id)];
            }
            const double depth = sum / static_cast<double>(center.corners.size());
            center.elevation = std::max(config.sea_level, center.elevation - depth);
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_VALLEYS_H
