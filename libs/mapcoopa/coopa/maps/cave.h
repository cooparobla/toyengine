/**
 * @file cave.h
 * @brief What part of a cave system a passage belongs to, and its stable
 *        serialisation name.
 */

#ifndef COOPA_MAPS_CAVE_H
#define COOPA_MAPS_CAVE_H

#include <cstddef>
#include <string_view>

namespace coopa {
namespace maps {

/**
 * @enum CaveZone
 * @brief Which half of a cave system a passage sits in, above or below the water table.
 *
 * Not decoration: this is the one thing the growth model branches on, and it is
 * why two stretches of the same cave look nothing alike. Above the water table
 * water is falling under gravity and cuts a steep, narrow way down with few
 * junctions; at and below it the water is moving sideways through rock already
 * full of it, and cuts wide, level, heavily branched passage instead.
 *
 * Recording it rather than inferring it from the floor height means the renderer
 * and a consumer cannot disagree about which regime a passage was cut in --
 * inference would have to re-derive the system's own water table, which is a
 * property of where its mouth opened and not of the passage in hand.
 *
 * Names are append-only, exactly as `Biome`'s and `BuildingRole`'s are: a zone's
 * position is not its identity, and inserting above would silently reinterpret
 * every cave in every saved map.
 */
enum class CaveZone {
    Vadose,
    Phreatic
};

/** @brief Number of distinct `CaveZone` values; the size of any zone-indexed table. */
inline constexpr std::size_t k_cave_zone_count = 2;

/**
 * @brief Maps a cave zone to its serialisation name.
 * @param zone The zone to name.
 * @return A `snake_case` identifier, e.g. `"phreatic"`.
 */
inline std::string_view cave_zone_name(CaveZone zone) {
    switch (zone) {
        case CaveZone::Vadose:   return "vadose";
        case CaveZone::Phreatic: return "phreatic";
    }
    return "vadose";
}

/**
 * @brief Resolves a serialisation name back to a zone.
 * @param name A name previously produced by `cave_zone_name()`.
 * @return The matching zone, or `CaveZone::Vadose` if the name is unknown.
 */
inline CaveZone cave_zone_from_name(std::string_view name) {
    for (std::size_t i = 0; i < k_cave_zone_count; ++i) {
        const CaveZone candidate = static_cast<CaveZone>(i);
        if (cave_zone_name(candidate) == name) {
            return candidate;
        }
    }
    return CaveZone::Vadose;
}

/**
 * @enum CaveFeature
 * @brief What a single node of a cave is, which is what makes a system read as one.
 *
 * A cave drawn as one unvarying tube is a pipe. These are the four shapes a
 * passage actually takes, and each is produced by a different thing happening:
 *
 * - `Passage` is ordinary going, and the overwhelming majority.
 * - `Chamber` is where passages meet or where one ends, widened because that is
 *   where the rock has been worked at from more than one direction.
 * - `Shaft` is a vertical pitch -- water falling rather than flowing, which is
 *   what puts a drop in the middle of an otherwise walkable cave.
 * - `Sump` is where a passage ends against the constraint it could not pass:
 *   the roof coming down, or the water table it cannot be cut below.
 *
 * Names are append-only, for the same reason `CaveZone`'s are.
 */
enum class CaveFeature {
    Passage,
    Chamber,
    Shaft,
    Sump
};

/** @brief Number of distinct `CaveFeature` values; the size of any feature-indexed table. */
inline constexpr std::size_t k_cave_feature_count = 4;

/**
 * @brief Maps a cave feature to its serialisation name.
 * @param feature The feature to name.
 * @return A `snake_case` identifier, e.g. `"chamber"`.
 */
inline std::string_view cave_feature_name(CaveFeature feature) {
    switch (feature) {
        case CaveFeature::Passage: return "passage";
        case CaveFeature::Chamber: return "chamber";
        case CaveFeature::Shaft:   return "shaft";
        case CaveFeature::Sump:    return "sump";
    }
    return "passage";
}

/**
 * @brief Resolves a serialisation name back to a feature.
 * @param name A name previously produced by `cave_feature_name()`.
 * @return The matching feature, or `CaveFeature::Passage` if the name is unknown.
 */
inline CaveFeature cave_feature_from_name(std::string_view name) {
    for (std::size_t i = 0; i < k_cave_feature_count; ++i) {
        const CaveFeature candidate = static_cast<CaveFeature>(i);
        if (cave_feature_name(candidate) == name) {
            return candidate;
        }
    }
    return CaveFeature::Passage;
}

/**
 * @brief Whether a feature is somewhere a passage widens out rather than merely runs.
 *
 * The one definition of "is this a room", so the renderer, the field builder and
 * a consumer placing geometry cannot disagree about which nodes are the spaces of
 * a cave and which are the going between them.
 *
 * @param feature The feature to test.
 * @return True for a chamber, false for anything else.
 */
inline bool is_open_feature(CaveFeature feature) { return feature == CaveFeature::Chamber; }

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_CAVE_H
