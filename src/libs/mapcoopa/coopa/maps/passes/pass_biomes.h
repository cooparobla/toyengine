/**
 * @file pass_biomes.h
 * @brief Sixth pass: classifies every cell from its elevation and moisture.
 */

#ifndef COOPA_MAPS_PASSES_PASS_BIOMES_H
#define COOPA_MAPS_PASSES_PASS_BIOMES_H

#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @class PassBiomes
 * @brief Assigns `MapCenter::biome`.
 *
 * The classification itself lives in `classify_biome()` so it can be tested on
 * a table of elevation and moisture values without building a map to reach it.
 */
class PassBiomes {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassMoisture` to have run.
     * @param config Unused; present so every pass shares one signature.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        (void)config;
        logger.info("map pass: biomes");

        for (MapCenter& center : graph.centers) {
            // Land-relative, because the Whittaker rows are described in terms of
            // *land*: the alpine band begins four fifths of the way up the
            // mountains, not four fifths of the way up from the sea bed. Passing
            // the raw height would silently shift every row once the waterline
            // moved off zero.
            center.biome = classify_biome(land_height(config, center.elevation),
                                          center.moisture, center.temperature,
                                          center.water, center.ocean, center.coast);
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_BIOMES_H
