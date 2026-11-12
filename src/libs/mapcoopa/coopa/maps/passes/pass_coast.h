/**
 * @file pass_coast.h
 * @brief Second pass: marks the shoreline, where land cells meet ocean cells.
 */

#ifndef COOPA_MAPS_PASSES_PASS_COAST_H
#define COOPA_MAPS_PASSES_PASS_COAST_H

#include <cstddef>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @class PassCoast
 * @brief Assigns `coast` to cells and refines `ocean`, `coast` and `water` on corners.
 *
 * A land cell is coast when any neighbour is ocean. A corner's state then
 * follows from the cells it touches: ocean when all of them are, coast when it
 * touches both land and ocean, water when it is neither wholly land nor a
 * coast.
 */
class PassCoast {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassWater` to have run.
     * @param config Unused; present so every pass shares one signature.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        (void)config;
        logger.info("map pass: coast");

        for (MapCenter& center : graph.centers) {
            if (center.border || center.water) {
                continue;
            }
            for (const CenterId neighbor_id : center.neighbors) {
                if (graph.centers[static_cast<std::size_t>(neighbor_id)].ocean) {
                    center.coast = true;
                    break;
                }
            }
        }

        for (MapCorner& corner : graph.corners) {
            std::size_t num_ocean = 0;
            std::size_t num_land = 0;
            for (const CenterId center_id : corner.touches) {
                const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
                if (center.ocean) ++num_ocean;
                if (!center.water) ++num_land;
            }

            corner.ocean = (num_ocean == corner.touches.size());
            corner.coast = (num_ocean > 0) && (num_land > 0);
            corner.water = corner.border || ((num_land != corner.touches.size()) && !corner.coast);
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_COAST_H
