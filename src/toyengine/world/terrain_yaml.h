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

#include <functional>
#include <string>

#include <fkYAML/node.hpp>

#include <coopa/scene/scene_loader.h>

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
 * Ties and silence both read as `Tiles`, which is what the bare name `Terrain` means.
 * Note that `layers:` is NOT a signal: both modes have it.
 */
TerrainMode infer_terrain_mode(const fkyaml::node& node);

/**
 * @brief Loads one tile set onto a terrain as a style, and returns that style's index.
 *
 * A tile set is an object asset whose children are the pieces, each found by its NAME
 * (`tile_piece_name()`) and taken from that child's `MeshRenderer`'s `mesh_path`; the set's own
 * `round_kind_corners` comes with it. The older spelling -- a bare mesh prefix, pieces at
 * `meshes/<prefix>_<piece>.yaml` -- is taken when `value` does not start with `objects/`.
 *
 * The piece loads are asynchronous: `TerrainComponent::poll_tile_sets()` is what waits for them
 * and bakes the library, and a terrain whose styles are empty never becomes `is_ready()` and so
 * builds no chunks at all. That is why a terrain assembled in code rather than parsed from a
 * scene has to come through here too -- setting `tile_set` alone loads nothing.
 *
 * @param terrain  The terrain to add the style to; its `styles` grows by one.
 * @param assets   Asset manager the pieces load through.
 * @param base_dir Directory relative paths resolve against (a scene's own folder, or empty).
 * @param name     What to call the style in log lines -- the set's path, or a legacy style name.
 * @param value    The tile-set object path, or a legacy mesh prefix.
 * @return The new style's index.
 */
int add_terrain_tile_set(TerrainComponent& terrain, coopa::asset::AssetManager& assets,
                         const std::string& base_dir, const std::string& name,
                         const std::string& value);

/**
 * @brief The index of the style already added under `name`, or -1.
 *
 * @param terrain The terrain to search.
 * @param name    The style name given to `add_terrain_tile_set()`.
 * @return The index, or -1 when no style carries that name.
 */
int terrain_style_named(const TerrainComponent& terrain, const std::string& name);

/**
 * @brief Gives a tile terrain its material, defaulting the surface palette atlas in.
 *
 * The chunks' shader reads each surface kind's colour from `textures/terrain_atlas.png`, so a
 * material that names no albedo map draws the whole world white -- which is what a freshly added
 * `TileTerrain`, or one built in code, would otherwise get. `authored` is the scene's own
 * `material:` value and may be an empty mapping; a material ASSET reference (a string, or a
 * mapping with `base:`) is taken as written and nothing is stamped over it.
 *
 * Goes through the same parser every other textured material in the engine uses, so the atlas
 * gets the sRGB colour-space declaration and the async load path with it.
 *
 * @param terrain  The terrain whose `material` is written.
 * @param authored The scene's `material:` node, or an empty mapping.
 * @param assets   Asset manager the textures load through.
 * @param ctx      Parse context for resolving relative paths; a default-constructed one resolves
 *                 against the project's asset roots, which is what a code-built terrain wants.
 */
void apply_tile_material(TerrainComponent& terrain, const fkyaml::node& authored,
                         coopa::asset::AssetManager& assets,
                         const coopa::scene::SceneLoader::ParseContext& ctx);

/**
 * @brief The stock tile material, for a terrain assembled in code rather than parsed.
 *
 * `apply_tile_material()` with nothing authored.
 *
 * @param terrain  The terrain whose `material` is written.
 * @param assets   Asset manager the textures load through.
 * @param base_dir Directory relative paths resolve against; empty uses the asset roots.
 */
void apply_default_tile_material(TerrainComponent& terrain, coopa::asset::AssetManager& assets,
                                 const std::string& base_dir = {});

/**
 * @brief Extra keys a project adds to a terrain, applied after the engine has parsed its own.
 *
 * The engine parses the keys it models and no others. A project whose world has a shape the
 * engine does not describe -- and so keys the engine has never heard of -- registers one of
 * these and reads them itself, installing whatever `ChunkShaper`/`TileGrid` they imply. It runs
 * with the component fully built, so it can both read the engine's parsed values and override
 * them.
 *
 * One hook, set from a project's `TOY_MODULE` (toyengine/core/module.h). Unset means a terrain
 * is exactly what the engine parsed.
 */
using TerrainParseHook = std::function<void(const fkyaml::node& node, TerrainComponent& terrain)>;

/** @brief Installs the project's terrain key reader; pass `{}` to remove it. */
void set_terrain_parse_hook(TerrainParseHook hook);

/** @brief The installed hook, or an empty function. */
const TerrainParseHook& terrain_parse_hook();

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
