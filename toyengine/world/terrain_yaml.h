/**
 * @file terrain_yaml.h
 * @brief Scene-file helpers both terrain modes' parsers share: which mode a block describes, and
 *        its `layers:`.
 *
 * One component serves both modes (terrain_component.h) and they are authored under three names
 * -- `Terrain`, `TileTerrain`, `HeightmapTerrain` -- so deciding the mode and reading the shared
 * surface vocabulary belong in one place rather than in each parser.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_YAML_H
#define TOYENGINE_WORLD_TERRAIN_YAML_H

#include <string>

#include <fkYAML/node.hpp>

#include <toyengine/world/terrain_types.h>

namespace coopa {
namespace asset {
class AssetManager;
}
} // namespace coopa

namespace toy {
namespace world {

class TerrainComponent;

/**
 * @brief Which mode a terrain block describes.
 *
 * An explicit `mode: tiles | heightmap` always wins. Without one the keys decide, because the
 * two halves share almost no spelling: `heightmap`, `splat0`, `splat1`, `resolution`, `size` or
 * `generate` mean a smooth grid, while `tile_set`, `tiles`, `generator`, `world_tiles`,
 * `tile_size`, `height_step` or `face_tiles` mean a tile world.
 *
 * Ties and silence both read as `Tiles`, which is what the bare name `Terrain` has always meant.
 * Note that `layers:` is NOT a signal: both modes have it.
 */
TerrainMode infer_terrain_mode(const fkyaml::node& node);

/**
 * @brief Reads `node`'s `layers:` sequence into `terrain`, resolving each layer's textures.
 *
 * Fills `terrain.layers` and the matching `layer_albedo_paths` / `layer_normal_paths`. The cap
 * follows the mode: a `Heightmap` terrain blends its layers through two RGBA8 splat maps and so
 * reaches `k_heightmap_layer_count`, while a `Tiles` terrain names one per face through a 4-bit
 * nibble and reaches `k_terrain_layer_count`. Anything past the cap is dropped with one warning.
 *
 * What an empty list means also follows the mode, and the difference is deliberate:
 *
 * - `Heightmap` gets one neutral grey-green layer, because a smooth surface has to show
 *   something and there is no palette behind it.
 * - `Tiles` is left **empty**, which is how a tile world asks for the built-in set
 *   (`builtin_tile_layers()`) -- the thirteen TileKinds against the palette atlas. That is what
 *   keeps a tile scene that never mentions layers looking exactly as it always has.
 *
 * @param base The scene's directory, for resolving relative texture paths.
 */
void parse_terrain_layers(const fkyaml::node& node, const std::string& base,
                          coopa::asset::AssetManager& assets, TerrainComponent& terrain);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_YAML_H
