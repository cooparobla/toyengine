/**
 * @file building.h
 * @brief What a building in a settlement is *for*, and its stable serialisation name.
 */

#ifndef COOPA_MAPS_BUILDING_H
#define COOPA_MAPS_BUILDING_H

#include <cstddef>
#include <string_view>

namespace coopa {
namespace maps {

/**
 * @enum BuildingRole
 * @brief What a building is for, which is what makes a settlement read as one.
 *
 * Without roles every footprint would be the same object: a square of the same
 * size in the same colour, so a capital of seventy buildings would have no hall,
 * no market and no centre -- only more squares than a village. A role is what
 * lets a reader tell the difference, and what lets a consumer place the right mesh.
 *
 * `Dwelling` is the default and the overwhelming majority; the rest are the civic
 * core, granted by tier and placed nearest the square.
 *
 * Names are append-only, exactly as `Biome`'s are: a role's position is not its
 * identity, but inserting above would silently reinterpret every settlement in
 * every saved map.
 */
enum class BuildingRole {
    Dwelling,
    Hall,
    Temple,
    Market,
    Inn,
    Smithy,
    Granary,
    Well,
    Barracks,
    Warehouse
};

/** @brief Number of distinct `BuildingRole` values; the size of any role-indexed table. */
inline constexpr std::size_t k_building_role_count = 10;

/**
 * @brief Maps a building role to its serialisation name.
 * @param role The role to name.
 * @return A `snake_case` identifier, e.g. `"hall"`.
 */
inline std::string_view building_role_name(BuildingRole role) {
    switch (role) {
        case BuildingRole::Dwelling:  return "dwelling";
        case BuildingRole::Hall:      return "hall";
        case BuildingRole::Temple:    return "temple";
        case BuildingRole::Market:    return "market";
        case BuildingRole::Inn:       return "inn";
        case BuildingRole::Smithy:    return "smithy";
        case BuildingRole::Granary:   return "granary";
        case BuildingRole::Well:      return "well";
        case BuildingRole::Barracks:  return "barracks";
        case BuildingRole::Warehouse: return "warehouse";
    }
    return "dwelling";
}

/**
 * @brief Resolves a serialisation name back to a role.
 * @param name A name previously produced by `building_role_name()`.
 * @return The matching role, or `BuildingRole::Dwelling` if the name is unknown.
 */
inline BuildingRole building_role_from_name(std::string_view name) {
    for (std::size_t i = 0; i < k_building_role_count; ++i) {
        const BuildingRole candidate = static_cast<BuildingRole>(i);
        if (building_role_name(candidate) == name) {
            return candidate;
        }
    }
    return BuildingRole::Dwelling;
}

/**
 * @brief Whether a role belongs to the civic core rather than being a home.
 *
 * The one definition of "is this a key structure", so the renderer, the packer
 * and a consumer cannot disagree about which buildings are the landmarks of a
 * settlement.
 *
 * @param role The role to test.
 * @return True for anything but a dwelling.
 */
inline bool is_civic_role(BuildingRole role) { return role != BuildingRole::Dwelling; }

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_BUILDING_H
