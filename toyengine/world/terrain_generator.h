/**
 * @file terrain_generator.h
 * @brief A TileTerrain's mapcoopa settings in scene YAML: the `generator:` block, read and
 *        written as mapcoopa's own config keys, on top of mapcoopa's standard map.
 *
 * A terrain stores only what differs from coopa::maps::standard_config() -- the map a bare
 * `mapcoopa` run makes -- so a new terrain is that map, and a scene says exactly what it changed.
 * Keys are map_yaml.h's (`sea_level`, `shape: { shape: archipelago }`, `towns: {...}`), so a
 * config file written for the CLI pastes straight in. Older scenes' flat component keys (`seed`,
 * `shape`, `sea_level`...) still load; the editor's World Generator moves them into the block.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_GENERATOR_H
#define TOYENGINE_WORLD_TERRAIN_GENERATOR_H

#include <array>

#include <fkYAML/node.hpp>

#include <coopa/maps/map_config.h>

namespace toy {
namespace world {

/** @brief The flat generator keys TileTerrain had before the `generator:` block. */
inline constexpr std::array<const char*, 11> k_legacy_generator_keys = {
    "seed", "grid_size", "sea_level", "terrain_roughness", "river_count", "shape",
    "continent_count", "continent_size_m", "irregularity", "coast_detail", "temperature_offset"};

/**
 * @brief The authored generator settings a TileTerrain component node describes: mapcoopa's
 *        standard map, then any legacy flat keys, then the `generator:` block.
 */
coopa::maps::MapConfig generator_config_from_node(const fkyaml::node& component);

/**
 * @brief The `generator:` block for `authored`: only the keys whose values differ from the
 *        standard map (nested blocks likewise), or an empty mapping when nothing does.
 */
fkyaml::node generator_node(const coopa::maps::MapConfig& authored);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_GENERATOR_H
