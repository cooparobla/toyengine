/**
 * @file pass_temperature.h
 * @brief Assigns a climate to every corner and cell, from latitude, altitude
 *        and a noise field.
 */

#ifndef COOPA_MAPS_PASSES_PASS_TEMPERATURE_H
#define COOPA_MAPS_PASSES_PASS_TEMPERATURE_H

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>

namespace coopa {
namespace maps {

/**
 * @class PassTemperature
 * @brief Assigns `temperature` to every corner and cell.
 *
 * Without this the map has no climate: biomes were classified on elevation and
 * moisture alone, so a desert was as likely at the pole as at the equator and
 * whole swathes of the biome table were unreachable. Latitude runs along the y
 * axis, so a generated world reads as a north-south slice of a globe -- cold at
 * both edges, warm through the middle.
 *
 * Four terms combine: a latitude band, a lapse rate that cools high ground, a
 * noise field that keeps isotherms from running as straight lines, and a global
 * offset that moves the whole world warmer or colder.
 *
 * The latitude band names its polar caps rather than implying them. A plain
 * `1 - d^falloff` does produce caps -- at the default exponent the ground freezes
 * beyond 87% of the way to the pole, the outer 6.5% of the map -- but nothing in
 * the configuration would say 6.5%, and no value of the exponent says zero.
 * `polar_extent_north` and `polar_extent_south` say it outright, one per pole.
 */
class PassTemperature {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassElevation` to have run.
     * @param config Supplies the lapse rate and the temperature noise field.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: temperature");

        const Noise noise(config.noise_temperature);
        const double grid_size = static_cast<double>(config.grid_size);
        if (grid_size <= 0.0) {
            return;
        }

        for (MapCorner& corner : graph.corners) {
            corner.temperature = graph.is_sphere() ? sample_sphere_(noise, config, graph, corner.point, corner.elevation)
                                                   : sample_(noise, config, grid_size, corner.point, corner.elevation);
        }

        for (MapCenter& center : graph.centers) {
            if (center.corners.empty()) {
                center.temperature =
                    graph.is_sphere() ? sample_sphere_(noise, config, graph, center.point, center.elevation)
                                      : sample_(noise, config, grid_size, center.point, center.elevation);
                continue;
            }
            double sum = 0.0;
            for (const CornerId corner_id : center.corners) {
                sum += graph.corners[static_cast<std::size_t>(corner_id)].temperature;
            }
            center.temperature = sum / static_cast<double>(center.corners.size());
        }
    }

private:
    /**
     * @brief Temperature at a point of a globe: latitude from height above the equator.
     *
     * `|z| / R` rather than the latitude angle, so a polar cap reaching `polar_extent` of the way
     * to the equator covers that fraction of its hemisphere's area -- what the setting means on a
     * plane. North is +z.
     */
    static double sample_sphere_(const Noise& noise, const MapConfig& config, const MapGraph& graph,
                                 const MapPoint& point, double elevation) {
        const double distance = std::clamp(std::abs(point.z) / graph.sphere_radius, 0.0, 1.0);
        const double extent = point.z >= 0.0 ? config.polar_extent_north : config.polar_extent_south;
        const double band = latitude_band_(distance, extent, config.temperature_falloff);
        const double lapse = elevation * config.temperature_lapse_rate;
        const double variation = noise.sample(point.x, point.y, point.z) * k_variation_scale;
        return std::clamp(band - lapse + variation + config.temperature_offset, 0.0, 1.0);
    }

    /** @brief Strength of the noise term, as a fraction of the full range. */
    static constexpr double k_variation_scale = 0.12;

    /**
     * @brief The latitude term, before altitude, noise and the global offset.
     *
     * Piecewise about the inner edge of the polar cap, and anchored to
     * `k_biome_frigid` there so that "polar extent" means the fraction of the map
     * that actually classifies as frozen rather than an abstract coefficient.
     *
     * With `extent` of zero the cap vanishes: `polar_d` is 1, the first branch is
     * unreachable, and the curve spans freezing to equatorial across the whole
     * hemisphere. The coldest latitude is then exactly freezing, so latitude alone
     * never selects ice -- altitude still can, which is what should happen.
     *
     * @param distance Distance from the equator, 0 at the middle and 1 at a pole.
     * @param extent Polar fraction of the map for this hemisphere, 0 to 0.5.
     * @param falloff Shapes the temperate half of the curve.
     * @return Warmth in `[0, 1]` from latitude alone.
     */
    static double latitude_band_(double distance, double extent, double falloff) {
        const double cap_width = 2.0 * std::clamp(extent, 0.0, 0.5);
        const double polar_d = 1.0 - cap_width;
        if (polar_d <= 0.0) {
            // The whole hemisphere is cap: ramp straight from freezing to nothing.
            return k_biome_frigid * (1.0 - distance);
        }
        // The width guard is not defensive: with no cap at all `polar_d` is exactly
        // 1, and a point exactly at the pole has `distance` exactly 1, so the ramp
        // below would divide zero by zero and hand back a NaN temperature for the
        // whole border ring.
        if (cap_width > 0.0 && distance >= polar_d) {
            return k_biome_frigid * (1.0 - distance) / cap_width;
        }
        return k_biome_frigid
             + (1.0 - k_biome_frigid) * (1.0 - std::pow(distance / polar_d, falloff));
    }

    /**
     * @brief Temperature at one point.
     *
     * @param noise The configured variation field.
     * @param config Supplies the lapse rate.
     * @param grid_size Map extent, for normalising latitude.
     * @param point Position in grid units.
     * @param elevation Height in `[0, 1]`.
     * @return Warmth in `[0, 1]`; 0 polar, 1 equatorial.
     */
    static double sample_(const Noise& noise, const MapConfig& config, double grid_size,
                          const MapPoint& point, double elevation) {
        const double latitude = std::clamp(point.y / grid_size, 0.0, 1.0);
        // 0 at the equator, 1 at either pole. Which pole decides whose cap applies:
        // the two are independent, so a world can carry ice at one end only.
        const double distance = std::abs(2.0 * latitude - 1.0);
        const double extent =
            latitude < 0.5 ? config.polar_extent_north : config.polar_extent_south;

        const double band = latitude_band_(distance, extent, config.temperature_falloff);
        const double lapse = elevation * config.temperature_lapse_rate;
        const double variation = noise.sample(point.x, point.y) * k_variation_scale;
        return std::clamp(band - lapse + variation + config.temperature_offset, 0.0, 1.0);
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_TEMPERATURE_H
