/**
 * @file terrain_yaml_test.cpp
 * @brief Which mode a terrain block describes, decided from its keys.
 *
 * One parser is registered under `Terrain`, `TileTerrain` and `HeightmapTerrain`, so this
 * function is what actually chooses the backend a scene gets. It runs before any asset exists,
 * which is why it can be tested on bare YAML nodes.
 */

#include <coopa/testing/test.h>

#include <fkYAML/node.hpp>
#include <toyengine/world/terrain_yaml.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_yaml");

using toy::world::infer_terrain_mode;
using toy::world::TerrainMode;

namespace {

/** @brief A mapping node with one key set to a harmless value. */
fkyaml::node with_key(const char* key) {
    fkyaml::node n = fkyaml::node::mapping();
    n[key] = fkyaml::node(1);
    return n;
}

} // namespace

/** @brief An explicit `mode:` always wins, whatever else the block says. */
COOPA_TEST(an_explicit_mode_wins) {
    fkyaml::node n = fkyaml::node::mapping();
    n["mode"] = fkyaml::node("heightmap");
    // Tile keys alongside it, which would otherwise decide.
    n["tile_set"] = fkyaml::node("objects/terrain/tileset_round");
    n["tiles"] = fkyaml::node("terrain/x.tiles");
    expect(infer_terrain_mode(n) == TerrainMode::Heightmap, "mode: heightmap wins over tile keys");

    fkyaml::node m = fkyaml::node::mapping();
    m["mode"] = fkyaml::node("tiles");
    m["heightmap"] = fkyaml::node("terrain/h.png");
    expect(infer_terrain_mode(m) == TerrainMode::Tiles, "mode: tiles wins over heightmap keys");
}

/** @brief Without a `mode:`, each half's own keys choose it. */
COOPA_TEST(the_keys_choose_the_mode) {
    for (const char* key : {"tile_set", "tiles", "generator", "world_tiles", "tiles_per_grid_unit",
                            "tile_size", "height_step", "height_scale", "face_tiles", "planet"}) {
        expect(infer_terrain_mode(with_key(key)) == TerrainMode::Tiles,
               std::string("`") + key + "` means a tile world");
    }
    for (const char* key : {"heightmap", "splat0", "splat1", "resolution", "splat_resolution", "size",
                            "generate", "tessellation", "patch_resolution"}) {
        expect(infer_terrain_mode(with_key(key)) == TerrainMode::Heightmap,
               std::string("`") + key + "` means a smooth grid");
    }
}

/**
 * @brief An empty block, and a block whose only key is shared, read as Tiles.
 *
 * `Terrain` with nothing under it has always meant the tile world, and `layers:` is no signal at
 * all now that both modes carry it -- reading it as heightmap would flip the mode of any tile
 * scene that authored its own surfaces.
 */
COOPA_TEST(silence_and_shared_keys_read_as_tiles) {
    expect(infer_terrain_mode(fkyaml::node::mapping()) == TerrainMode::Tiles, "an empty block is a tile world");
    expect(infer_terrain_mode(with_key("layers")) == TerrainMode::Tiles,
           "`layers:` belongs to both modes, so it decides nothing");
    expect(infer_terrain_mode(with_key("collision")) == TerrainMode::Tiles, "so does `collision:`");
    expect(infer_terrain_mode(with_key("physics_material")) == TerrainMode::Tiles,
           "...and `physics_material:`");
    expect(infer_terrain_mode(with_key("shadow_distance")) == TerrainMode::Tiles,
           "...and `shadow_distance:`");

    fkyaml::node n = fkyaml::node::mapping();
    n["mode"] = fkyaml::node("nonsense");
    expect(infer_terrain_mode(n) == TerrainMode::Tiles, "an unknown mode name falls back to tiles");
}

/** @brief A tile block keeps reading as tiles once it also authors layers. */
COOPA_TEST(a_tile_block_with_layers_stays_tiles) {
    fkyaml::node n = fkyaml::node::mapping();
    n["tile_set"] = fkyaml::node("objects/terrain/tileset_voxel");
    n["layers"] = fkyaml::node::sequence();
    expect(infer_terrain_mode(n) == TerrainMode::Tiles, "tile_set + layers is still a tile world");
}
