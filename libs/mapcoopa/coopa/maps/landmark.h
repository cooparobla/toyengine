/**
 * @file landmark.h
 * @brief Notable places: what kinds exist, their names, and which terrain each
 *        one can plausibly occupy.
 */

#ifndef COOPA_MAPS_LANDMARK_H
#define COOPA_MAPS_LANDMARK_H

#include <cstddef>
#include <string>
#include <string_view>

#include <coopa/maps/biome.h>

namespace coopa {
namespace maps {

/**
 * @enum LandmarkKind
 * @brief The sort of notable place a landmark is.
 *
 * Split between features the terrain produced and works someone left behind,
 * so a map carries a past as well as a present.
 */
enum class LandmarkKind {
    // --- Natural, read off the terrain ---
    Peak,
    Volcano,
    Crater,
    Waterfall,
    Canyon,
    Cape,
    Oasis,
    GreatLake,
    Glacier,
    HotSpring,
    // --- Left behind ---
    Ruins,
    StandingStones,
    Monolith,
    Wreck,
    Tower,
    Shrine,
    // --- One per region ---
    Wonder
};

/** @brief Number of distinct `LandmarkKind` values. */
inline constexpr std::size_t k_landmark_kind_count = 17;

/**
 * @brief Maps a landmark kind to its serialisation name.
 * @param kind The kind to name.
 * @return A `snake_case` identifier, e.g. `"standing_stones"`.
 */
inline std::string_view landmark_kind_name(LandmarkKind kind) {
    switch (kind) {
        case LandmarkKind::Peak:           return "peak";
        case LandmarkKind::Volcano:        return "volcano";
        case LandmarkKind::Crater:         return "crater";
        case LandmarkKind::Waterfall:      return "waterfall";
        case LandmarkKind::Canyon:         return "canyon";
        case LandmarkKind::Cape:           return "cape";
        case LandmarkKind::Oasis:          return "oasis";
        case LandmarkKind::GreatLake:      return "great_lake";
        case LandmarkKind::Glacier:        return "glacier";
        case LandmarkKind::HotSpring:      return "hot_spring";
        case LandmarkKind::Ruins:          return "ruins";
        case LandmarkKind::StandingStones: return "standing_stones";
        case LandmarkKind::Monolith:       return "monolith";
        case LandmarkKind::Wreck:          return "wreck";
        case LandmarkKind::Tower:          return "tower";
        case LandmarkKind::Shrine:         return "shrine";
        case LandmarkKind::Wonder:         return "wonder";
    }
    return "ruins";
}

/**
 * @brief Resolves a serialisation name back to a landmark kind.
 * @param name A name previously produced by `landmark_kind_name()`.
 * @return The matching kind, or `LandmarkKind::Ruins` if unknown.
 */
inline LandmarkKind landmark_kind_from_name(std::string_view name) {
    for (std::size_t i = 0; i < k_landmark_kind_count; ++i) {
        const LandmarkKind candidate = static_cast<LandmarkKind>(i);
        if (landmark_kind_name(candidate) == name) {
            return candidate;
        }
    }
    return LandmarkKind::Ruins;
}

/**
 * @brief The descriptive word a landmark's name is built around.
 * @param kind The kind to describe.
 * @return A capitalised noun, e.g. `"Falls"`, used as `"<name> <noun>"`.
 */
inline std::string_view landmark_noun(LandmarkKind kind) {
    switch (kind) {
        case LandmarkKind::Peak:           return "Peak";
        case LandmarkKind::Volcano:        return "Caldera";
        case LandmarkKind::Crater:         return "Crater";
        case LandmarkKind::Waterfall:      return "Falls";
        case LandmarkKind::Canyon:         return "Gorge";
        case LandmarkKind::Cape:           return "Cape";
        case LandmarkKind::Oasis:          return "Oasis";
        case LandmarkKind::GreatLake:      return "Mere";
        case LandmarkKind::Glacier:        return "Glacier";
        case LandmarkKind::HotSpring:      return "Springs";
        case LandmarkKind::Ruins:          return "Ruins";
        case LandmarkKind::StandingStones: return "Stones";
        case LandmarkKind::Monolith:       return "Monolith";
        case LandmarkKind::Wreck:          return "Wreck";
        case LandmarkKind::Tower:          return "Tower";
        case LandmarkKind::Shrine:         return "Shrine";
        case LandmarkKind::Wonder:         return "Sanctum";
    }
    return "Ruins";
}

/**
 * @brief Whether a landmark kind can plausibly stand on a given biome.
 *
 * Landmark vocabulary is gated by terrain rather than scattered uniformly: an
 * oasis in a glacier or a mangrove volcano reads as a bug, not as variety. A
 * kind with no terrain restriction returns true everywhere.
 *
 * @param kind The landmark kind.
 * @param biome The biome of the cell it would occupy.
 * @return True if the pairing is plausible.
 */
inline bool landmark_suits_biome(LandmarkKind kind, Biome biome) {
    switch (kind) {
        case LandmarkKind::Volcano:
            return biome == Biome::VolcanicField || biome == Biome::Scorched
                || biome == Biome::Badlands || biome == Biome::Bare;
        case LandmarkKind::Glacier:
            return biome == Biome::Glacier || biome == Biome::Ice || biome == Biome::Snow;
        case LandmarkKind::Oasis:
            return biome == Biome::SubtropicalDesert || biome == Biome::TemperateDesert
                || biome == Biome::ColdDesert || biome == Biome::Dunes
                || biome == Biome::SaltFlat;
        case LandmarkKind::Canyon:
            return biome == Biome::Badlands || biome == Biome::Scorched
                || biome == Biome::TemperateDesert || biome == Biome::SubtropicalDesert
                || biome == Biome::Steppe;
        case LandmarkKind::HotSpring:
            // Deliberately excludes tundra and moorland: both blanket huge areas,
            // and letting them qualify turns a rare feature into the commonest
            // thing on the map.
            return biome == Biome::VolcanicField || biome == Biome::Taiga
                || biome == Biome::AlpineMeadow || biome == Biome::Glacier;
        case LandmarkKind::Peak:
            return biome != Biome::Ocean && biome != Biome::Lake && biome != Biome::Swamp
                && biome != Biome::Marsh;
        case LandmarkKind::Wreck:
            return biome == Biome::Beach || biome == Biome::Mangrove || biome == Biome::SaltFlat;
        case LandmarkKind::StandingStones:
            return biome == Biome::Moorland || biome == Biome::Steppe || biome == Biome::Grassland
                || biome == Biome::Tundra || biome == Biome::Chaparral;
        case LandmarkKind::Shrine:
        case LandmarkKind::Tower:
        case LandmarkKind::Ruins:
        case LandmarkKind::Monolith:
        case LandmarkKind::Wonder:
        case LandmarkKind::Crater:
        case LandmarkKind::Waterfall:
        case LandmarkKind::Cape:
        case LandmarkKind::GreatLake:
            return true;
    }
    return true;
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_LANDMARK_H
