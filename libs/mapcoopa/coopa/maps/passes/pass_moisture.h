/**
 * @file pass_moisture.h
 * @brief Fifth pass: spreads wetness outward from fresh water and normalises
 *        it across the map.
 */

#ifndef COOPA_MAPS_PASSES_PASS_MOISTURE_H
#define COOPA_MAPS_PASSES_PASS_MOISTURE_H

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <queue>
#include <vector>
#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class PassMoisture
 * @brief Assigns `moisture` to every corner and cell.
 *
 * Lakes and rivers seed the field -- the ocean deliberately does not, so that
 * a desert can sit behind a coastal range. Wetness then diffuses outward
 * losing a tenth per step, ocean and coastal corners are pinned wet, and the
 * result is rank-normalised so every map spans the full moisture range no
 * matter how much fresh water it happened to grow.
 */
class PassMoisture {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassRivers` to have run.
     * @param config Unused; present so every pass shares one signature.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        (void)config;
        logger.info("map pass: moisture");

        assign_corner_moisture_(graph);
        redistribute_moisture_(graph);
        assign_center_moisture_(graph);
    }

private:
    /** @brief Fraction of wetness carried across one step of the diffusion. */
    static constexpr double k_moisture_decay = 0.9;
    /** @brief Wetness contributed per unit of river volume. */
    static constexpr double k_river_moisture_scale = 0.2;
    /** @brief Ceiling on a river's seeded wetness, before normalisation. */
    static constexpr double k_max_river_moisture = 3.0;

    /** @brief Seeds from fresh water and diffuses outward. */
    void assign_corner_moisture_(MapGraph& graph) const {
        std::queue<CornerId> pending;

        for (MapCorner& corner : graph.corners) {
            if ((corner.water || corner.river > 0) && !corner.ocean) {
                corner.moisture = corner.river > 0
                    ? std::min(k_max_river_moisture, k_river_moisture_scale * corner.river)
                    : 1.0;
                pending.push(corner.index);
            } else {
                corner.moisture = 0.0;
            }
        }

        while (!pending.empty()) {
            const CornerId current_id = pending.front();
            pending.pop();

            const double source_moisture = graph.corners[static_cast<std::size_t>(current_id)].moisture;
            for (const CornerId neighbor_id : graph.corners[static_cast<std::size_t>(current_id)].adjacent) {
                MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
                const double new_moisture = source_moisture * k_moisture_decay;
                if (new_moisture > neighbor.moisture) {
                    neighbor.moisture = new_moisture;
                    pending.push(neighbor.index);
                }
            }
        }

        for (MapCorner& corner : graph.corners) {
            if (corner.ocean || corner.coast) {
                corner.moisture = 1.0;
            }
        }
    }

    /**
     * @brief Rank-normalises wetness so it spans `[0, 1]` uniformly.
     *
     * Sorts an index array, not the corner storage: sorting `corners` itself
     * would break the `corners[i].index == i` invariant every later pass and
     * the serialiser depend on.
     */
    void redistribute_moisture_(MapGraph& graph) const {
        const std::size_t count = graph.corners.size();
        if (count < 2) {
            return;
        }

        std::vector<CornerId> order(count);
        std::iota(order.begin(), order.end(), static_cast<CornerId>(0));
        coopa::maps::sort(order.begin(), order.end(), [&graph](CornerId a, CornerId b) {
            return graph.corners[static_cast<std::size_t>(a)].moisture
                 < graph.corners[static_cast<std::size_t>(b)].moisture;
        });

        for (std::size_t rank = 0; rank < count; ++rank) {
            graph.corners[static_cast<std::size_t>(order[rank])].moisture =
                static_cast<double>(rank) / static_cast<double>(count - 1);
        }
    }

    /** @brief Averages corner wetness down onto their cells. */
    void assign_center_moisture_(MapGraph& graph) const {
        for (MapCenter& center : graph.centers) {
            if (center.corners.empty()) {
                center.moisture = 0.0;
                continue;
            }
            double sum = 0.0;
            for (const CornerId corner_id : center.corners) {
                MapCorner& corner = graph.corners[static_cast<std::size_t>(corner_id)];
                if (corner.moisture > 1.0) {
                    corner.moisture = 1.0;
                }
                sum += corner.moisture;
            }
            center.moisture = sum / static_cast<double>(center.corners.size());
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_MOISTURE_H
