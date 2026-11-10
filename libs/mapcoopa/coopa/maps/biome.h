/**
 * @file biome.h
 * @brief The Whittaker biome taxonomy, its stable string names, and the
 *        elevation x moisture classifier that assigns one to a map cell.
 */

#ifndef COOPA_MAPS_BIOME_H
#define COOPA_MAPS_BIOME_H

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace coopa {
namespace maps {

/**
 * @enum Biome
 * @brief The terrain classification assigned to every land or water cell.
 *
 * The set and the thresholds that produce it come from Amit Patel's
 * *Polygonal Map Generation* -- a coarse Whittaker diagram indexed by
 * elevation and moisture, with water, lake and coast handled as special cases
 * before the diagram is consulted.
 *
 * Stored as an enum rather than a `std::string`: the renderer looks a colour up
 * per cell per frame, and a string compare chain of twenty `==` tests would be
 * the hot path of that loop. The
 * strings survive as `biome_name()` for serialisation only.
 */
enum class Biome {
    Ocean,
    Lake,
    Marsh,
    Ice,
    Beach,
    Snow,
    Tundra,
    Bare,
    Scorched,
    Taiga,
    Shrubland,
    TemperateDesert,
    TemperateRainForest,
    TemperateDeciduousForest,
    Grassland,
    TropicalRainForest,
    TropicalSeasonalForest,
    SubtropicalDesert,
    // Appended, never reordered: a biome's position is not its identity, but
    // inserting above would silently reinterpret every palette written by index.
    AlpineMeadow,
    Glacier,
    ColdDesert,
    Steppe,
    Savanna,
    Chaparral,
    Moorland,
    BorealWetland,
    Swamp,
    Mangrove,
    CloudForest,
    Badlands,
    SaltFlat,
    Dunes,
    VolcanicField
};

/** @brief Number of distinct `Biome` values; the size of any biome-indexed table. */
inline constexpr std::size_t k_biome_count = 33;

/**
 * @brief Elevation below which land is basin floor: wetland if wet, salt flat if dry.
 *
 * A fraction of the *land* range rather than an absolute height -- `PassBiomes`
 * passes `land_height()`, so 0.12 is the lowest eighth of the ground above the
 * waterline, wherever the waterline happens to sit.
 */
inline constexpr double k_biome_basin_elevation = 0.12;
/** @brief Moisture above which basin floor is waterlogged rather than merely low. */
inline constexpr double k_biome_wetland_moisture = 0.7;
/** @brief Temperature below which the land is permanently frozen. */
inline constexpr double k_biome_frigid = 0.2;
/** @brief Temperature below which the land is boreal. */
inline constexpr double k_biome_cold = 0.4;
/** @brief Temperature above which the land is tropical. */
inline constexpr double k_biome_warm = 0.68;
/** @brief Temperature above which shores grow mangrove and lakes become swamp. */
inline constexpr double k_biome_tropical = 0.72;

/** @brief Elevation bands separating the alpine, montane and lowland rows of the diagram. */
inline constexpr double k_biome_alpine_elevation = 0.8;
inline constexpr double k_biome_montane_elevation = 0.6;
inline constexpr double k_biome_lowland_elevation = 0.3;

/**
 * @brief Maps a biome to its serialisation name.
 *
 * These `snake_case` names are written into the `.yaml` and are the stable
 * on-disk identity of a biome -- adding a value to `Biome` is safe, renaming
 * one of these strings invalidates every saved map.
 *
 * @param biome The biome to name.
 * @return A `snake_case` identifier, e.g. `"temperate_deciduous_forest"`.
 */
inline std::string_view biome_name(Biome biome) {
    switch (biome) {
        case Biome::Ocean:                    return "ocean";
        case Biome::Lake:                     return "lake";
        case Biome::Marsh:                    return "marsh";
        case Biome::Ice:                      return "ice";
        case Biome::Beach:                    return "beach";
        case Biome::Snow:                     return "snow";
        case Biome::Tundra:                   return "tundra";
        case Biome::Bare:                     return "bare";
        case Biome::Scorched:                 return "scorched";
        case Biome::Taiga:                    return "taiga";
        case Biome::Shrubland:                return "shrubland";
        case Biome::TemperateDesert:          return "temperate_desert";
        case Biome::TemperateRainForest:      return "temperate_rain_forest";
        case Biome::TemperateDeciduousForest: return "temperate_deciduous_forest";
        case Biome::Grassland:                return "grassland";
        case Biome::TropicalRainForest:       return "tropical_rain_forest";
        case Biome::TropicalSeasonalForest:   return "tropical_seasonal_forest";
        case Biome::SubtropicalDesert:        return "subtropical_desert";
        case Biome::AlpineMeadow:             return "alpine_meadow";
        case Biome::Glacier:                  return "glacier";
        case Biome::ColdDesert:               return "cold_desert";
        case Biome::Steppe:                   return "steppe";
        case Biome::Savanna:                  return "savanna";
        case Biome::Chaparral:                return "chaparral";
        case Biome::Moorland:                 return "moorland";
        case Biome::BorealWetland:            return "boreal_wetland";
        case Biome::Swamp:                    return "swamp";
        case Biome::Mangrove:                 return "mangrove";
        case Biome::CloudForest:              return "cloud_forest";
        case Biome::Badlands:                 return "badlands";
        case Biome::SaltFlat:                 return "salt_flat";
        case Biome::Dunes:                    return "dunes";
        case Biome::VolcanicField:            return "volcanic_field";
    }
    return "ocean";
}

/**
 * @brief Resolves a serialisation name back to its biome.
 *
 * Accepts `temperate_decidious_forest` as well as the correctly spelled
 * `temperate_deciduous_forest`: the original generator misspelled it, and maps
 * saved by that version should still load.
 *
 * @param name A name previously produced by `biome_name()`.
 * @return The matching biome, or `Biome::Ocean` if the name is unknown.
 */
inline Biome biome_from_name(std::string_view name) {
    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const Biome candidate = static_cast<Biome>(i);
        if (biome_name(candidate) == name) {
            return candidate;
        }
    }
    if (name == "temperate_decidious_forest") {
        return Biome::TemperateDeciduousForest;
    }
    return Biome::Ocean;
}

/**
 * @brief How readily a biome supports settlement and the traffic between
 *        settlements, from 0 to 1.
 *
 * Zero entries are hard exclusions -- nothing is built on open water, ice or
 * scorched rock. Grassland and deciduous forest score highest, which is what
 * pushes settlements onto the temperate middle of a continent rather than its
 * extremes.
 *
 * Lives here beside `classify_biome()` for the same reason: it is a pure table
 * and can be exercised on a list of inputs without building a whole map to
 * reach it. Both the road pass and the town pass read it, so where people live
 * and where the roads between them run cannot disagree about which ground is
 * hospitable.
 *
 * @param biome The biome to weigh.
 * @return Habitability in `[0, 1]`; 0 means nothing is ever built there.
 */
inline double biome_habitability(Biome biome) {
    switch (biome) {
        case Biome::Grassland:                return 1.00;
        case Biome::TemperateDeciduousForest: return 0.90;
        case Biome::Beach:                    return 0.75;
        case Biome::Shrubland:                return 0.65;
        case Biome::TemperateRainForest:      return 0.60;
        case Biome::TropicalSeasonalForest:   return 0.60;
        case Biome::Marsh:                    return 0.35;
        case Biome::Taiga:                    return 0.35;
        case Biome::TropicalRainForest:       return 0.30;
        case Biome::TemperateDesert:          return 0.25;
        case Biome::SubtropicalDesert:        return 0.20;
        case Biome::Tundra:                   return 0.15;
        case Biome::Bare:                     return 0.05;
        // Grazing and grain country, second only to the temperate lowlands.
        case Biome::Savanna:                  return 0.70;
        case Biome::Steppe:                   return 0.60;
        case Biome::Chaparral:                return 0.55;
        case Biome::Moorland:                 return 0.45;
        case Biome::CloudForest:              return 0.45;
        case Biome::Mangrove:                 return 0.40;
        case Biome::Swamp:                    return 0.30;
        case Biome::AlpineMeadow:             return 0.30;
        case Biome::BorealWetland:            return 0.25;
        case Biome::ColdDesert:               return 0.12;
        case Biome::Badlands:                 return 0.10;
        case Biome::Dunes:                    return 0.05;
        case Biome::SaltFlat:                 return 0.03;
        // Nothing is built on open water, ice, or bare volcanic rock.
        case Biome::Ocean:
        case Biome::Lake:
        case Biome::Ice:
        case Biome::Snow:
        case Biome::Glacier:
        case Biome::VolcanicField:
        case Biome::Scorched:                 return 0.0;
    }
    return 0.0;
}

/**
 * @brief Classifies a cell from its terrain state and climate.
 *
 * Water state wins over climate: an ocean cell is `Ocean` at any latitude, an
 * inland water cell is `Ice` or `Lake` and never anything else, and a land cell
 * touching the ocean is a shore. Only the remaining interior land consults the climate
 * diagram.
 *
 * That diagram is three-dimensional -- temperature, then elevation, then
 * moisture. Temperature comes first because latitude is the strongest control
 * on what grows: the same height and rainfall give taiga at the pole and rain
 * forest at the equator. Classifying on elevation and moisture alone, as the
 * original did, left a third of the table unreachable and put deserts at the
 * pole.
 *
 * Lives here rather than inside the biome pass so it can be exercised directly
 * on a table of inputs, without building a whole map to reach it.
 *
 * @param elevation Normalised height in `[0, 1]`.
 * @param moisture Normalised wetness in `[0, 1]`.
 * @param temperature Normalised warmth in `[0, 1]`; 0 polar, 1 equatorial.
 * @param is_water True if the cell is a lake or ocean.
 * @param is_ocean True if the cell is connected to the map border by water.
 * @param is_coast True if the cell is land bordering an ocean cell.
 * @return The assigned biome.
 */
inline Biome classify_biome(double elevation, double moisture, double temperature,
                            bool is_water, bool is_ocean, bool is_coast) {
    if (is_ocean) {
        return Biome::Ocean;
    }
    if (is_water) {
        // Every water cell gets a water biome, without exception. A cell with a
        // `water_level` is a body of water and has to *read* as one: handing a
        // shallow lake `Marsh` or `Swamp`, which are dark greens, would make rivers
        // on the biome and composite layers end in what looks like forest. Ending
        // in a lake you cannot see is indistinguishable from ending nowhere, and
        // "a river ends in water" is worth nothing if the map disagrees. `Marsh`
        // and `Swamp` are what the words mean -- wet *ground*, classified below.
        //
        // Freezing is on temperature alone. An elevation test here would
        // double-count altitude, because `PassTemperature` already applies an
        // altitude lapse rate -- a high lake would be frozen twice over and a cold
        // low one not at all. A tarn in a temperate zone is a lake.
        return temperature < k_biome_frigid ? Biome::Ice : Biome::Lake;
    }
    if (is_coast) {
        // A warm, wet shore grows mangrove; a cold one is bare shingle.
        if (temperature > k_biome_tropical && moisture > 0.6) return Biome::Mangrove;
        if (temperature < k_biome_frigid) return Biome::Tundra;
        return Biome::Beach;
    }

    // --- Wetland: waterlogged basin floor ---
    // Ahead of the climate bands because being under water most of the year
    // decides a biome more than latitude does. Moisture is seeded from lakes and
    // rivers, so this lands where it should: the low, wet ground beside water.
    // Frigid is excluded -- below freezing a basin is permafrost, not marsh, and
    // the frigid band below already answers for it.
    if (temperature >= k_biome_frigid && elevation < k_biome_basin_elevation
        && moisture > k_biome_wetland_moisture) {
        if (temperature < k_biome_cold) return Biome::BorealWetland;
        return temperature > k_biome_tropical ? Biome::Swamp : Biome::Marsh;
    }

    // --- Frigid: ice caps, glaciers and cold desert ---
    if (temperature < k_biome_frigid) {
        if (elevation > k_biome_alpine_elevation) return Biome::Glacier;
        if (moisture > 0.5) return Biome::Snow;
        if (moisture > 0.2) return Biome::Tundra;
        return Biome::ColdDesert;
    }

    // --- Cold: boreal ---
    if (temperature < k_biome_cold) {
        if (elevation > k_biome_alpine_elevation) {
            return moisture > 0.4 ? Biome::Snow : Biome::Bare;
        }
        if (elevation > k_biome_montane_elevation) {
            return moisture > 0.5 ? Biome::Taiga : Biome::Moorland;
        }
        if (moisture > 0.7) return Biome::BorealWetland;
        if (moisture > 0.4) return Biome::Taiga;
        if (moisture > 0.2) return Biome::Steppe;
        return Biome::ColdDesert;
    }

    // --- Temperate ---
    if (temperature < k_biome_warm) {
        if (elevation > k_biome_alpine_elevation) {
            if (moisture > 0.5) return Biome::AlpineMeadow;
            if (moisture > 0.25) return Biome::Bare;
            return Biome::Scorched;
        }
        if (elevation > k_biome_montane_elevation) {
            if (moisture > 0.7) return Biome::CloudForest;
            if (moisture > 0.4) return Biome::Shrubland;
            return Biome::TemperateDesert;
        }
        if (moisture > 0.83) return Biome::TemperateRainForest;
        if (moisture > 0.5) return Biome::TemperateDeciduousForest;
        if (moisture > 0.3) return Biome::Grassland;
        if (moisture > 0.16) return Biome::Chaparral;
        return Biome::TemperateDesert;
    }

    // --- Hot / tropical ---
    if (elevation > k_biome_alpine_elevation) {
        if (moisture > 0.5) return Biome::AlpineMeadow;
        if (moisture > 0.2) return Biome::Badlands;
        return Biome::VolcanicField;
    }
    if (elevation > k_biome_montane_elevation) {
        if (moisture > 0.66) return Biome::CloudForest;
        if (moisture > 0.33) return Biome::Savanna;
        return Biome::Badlands;
    }
    if (moisture > 0.75) return Biome::TropicalRainForest;
    if (moisture > 0.5) return Biome::TropicalSeasonalForest;
    if (moisture > 0.3) return Biome::Savanna;
    if (moisture > 0.12) return Biome::SubtropicalDesert;
    return elevation < k_biome_basin_elevation ? Biome::SaltFlat : Biome::Dunes;
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_BIOME_H
