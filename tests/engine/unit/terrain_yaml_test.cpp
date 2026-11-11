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

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/yaml/document.h>

#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/world/terrain_component.h>

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

namespace {

/**
 * @brief Picks a root object out of a loaded scene document by its `name:`, or an empty node.
 *
 * The `Terrain`/`MapGenerator`/`EnvPresetSwitcher` parsers only register under the
 * device-taking `register_scene_components()` overload (toyengine/scene/register.cpp), so a
 * device-free test cannot go through `SceneLoader::load()` and read the resulting components --
 * there would be none. Reading the YAML tree directly keeps this test exactly where
 * `infer_terrain_mode` (this file's own subject) already operates: before any component exists.
 */
fkyaml::node find_root_object(const fkyaml::node& doc, const std::string& name) {
    const fkyaml::node& roots = doc.at("scene").at("root_objects");
    for (const auto& obj : roots) {
        if (obj.contains("name") && obj.at("name").get_value<std::string>() == name) return obj;
    }
    return fkyaml::node();
}

/** @brief The first component of `type:` on an object node, or an empty node. */
fkyaml::node find_component(const fkyaml::node& obj, const std::string& type) {
    if (!obj.contains("components")) return fkyaml::node();
    for (const auto& c : obj.at("components")) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == type) return c;
    }
    return fkyaml::node();
}

/** @brief A named child object node, or an empty node. */
fkyaml::node find_child(const fkyaml::node& obj, const std::string& name) {
    if (!obj.contains("children")) return fkyaml::node();
    for (const auto& c : obj.at("children")) {
        if (c.contains("name") && c.at("name").get_value<std::string>() == name) return c;
    }
    return fkyaml::node();
}

/** @brief A string-valued key, or `fallback` if absent. */
std::string str_at(const fkyaml::node& n, const char* key, const std::string& fallback = "") {
    return n.contains(key) ? n.at(key).get_value<std::string>() : fallback;
}

/** @brief A number-valued key read as double, or `fallback` if absent (int or float either way). */
double num_at(const fkyaml::node& n, const char* key, double fallback = 0.0) {
    if (!n.contains(key)) return fallback;
    const fkyaml::node& v = n.at(key);
    return v.is_integer() ? static_cast<double>(v.get_value<std::int64_t>()) : v.get_value<double>();
}

bool bool_at(const fkyaml::node& n, const char* key, bool fallback = false) {
    return n.contains(key) ? n.at(key).get_value<bool>() : fallback;
}

glm::vec3 position_of(const fkyaml::node& obj) {
    glm::vec3 p{0.0f};
    if (!obj.contains("components")) return p;
    const fkyaml::node t = find_component(obj, "Transform");
    if (t.is_null() || !t.contains("position")) return p;
    const fkyaml::node& pos = t.at("position");
    p.x = static_cast<float>(num_at(pos, "x"));
    p.y = static_cast<float>(num_at(pos, "y"));
    p.z = static_cast<float>(num_at(pos, "z"));
    return p;
}

} // namespace

/**
 * @brief The shipped `unified_terrain_demo` scene: three `Terrain` blocks over one
 *        `MapGenerator`, asserted straight from the parsed YAML tree -- no device, no bake.
 */
COOPA_TEST(unified_terrain_demo_keeps_its_three_flavours_interchangeable) {
    const std::string path =
        std::string(ROOT_DIR) + "/assets/scenes/terrain/unified_terrain_demo/scene.yaml";
    const fkyaml::node doc = coopa::yaml::load_document(path);

    const fkyaml::node soft = find_root_object(doc, "terrain_soft");
    const fkyaml::node voxel = find_root_object(doc, "terrain_voxel");
    const fkyaml::node smooth = find_root_object(doc, "terrain_smooth");
    require(!soft.is_null() && !voxel.is_null() && !smooth.is_null(),
            "unified: all three terrain objects are in the scene");

    int other_terrains = 0;
    for (const auto& obj : doc.at("scene").at("root_objects")) {
        const std::string name = str_at(obj, "name");
        if (name == "terrain_soft" || name == "terrain_voxel" || name == "terrain_smooth") continue;
        if (!find_component(obj, "Terrain").is_null()) ++other_terrains;
    }
    expect(other_terrains == 0, "unified: no fourth Terrain hiding on another root object");

    expect(bool_at(soft, "active", true) && !bool_at(voxel, "active", true) &&
               !bool_at(smooth, "active", true),
           "unified: soft is the one authored active");

    const glm::vec3 p0 = position_of(soft);
    expect(p0 == position_of(voxel) && p0 == position_of(smooth),
           "unified: all three sit at the same Transform position");

    const fkyaml::node ts = find_component(soft, "Terrain");
    const fkyaml::node tv = find_component(voxel, "Terrain");
    const fkyaml::node th = find_component(smooth, "Terrain");
    require(!ts.is_null() && !tv.is_null() && !th.is_null(), "unified: each carries a Terrain block");
    expect(infer_terrain_mode(ts) == TerrainMode::Tiles && infer_terrain_mode(tv) == TerrainMode::Tiles,
           "unified: soft and voxel both read as tile terrains");
    expect(infer_terrain_mode(th) == TerrainMode::Heightmap, "unified: smooth reads as a heightmap");

    // The two tile terrains: same shared params, differing only in tile_set / kind_tile_sets.
    for (const char* key : {"chunk_size", "view_radius", "tile_size", "height_step",
                            "soil_depth_steps", "shadow_distance", "collision_radius"}) {
        expect(num_at(ts, key) == num_at(tv, key),
               std::string("unified: soft and voxel share `") + key + "`");
    }
    expect(bool_at(ts, "collision", true) == bool_at(tv, "collision", true),
           "unified: ...and `collision`");
    expect(str_at(ts, "tile_set") != str_at(tv, "tile_set"), "unified: they differ in tile_set");
    expect(!ts.contains("layers") && !tv.contains("layers"),
           "unified: neither tile terrain authors layers:");
    expect(ts.contains("kind_tile_sets") && ts.at("kind_tile_sets").contains("rock") &&
               ts.at("kind_tile_sets").contains("stone"),
           "unified: soft overrides rock and stone to the rounded set");
    expect(!tv.contains("kind_tile_sets"),
           "unified: voxel has no per-kind overrides -- a rounded set would soften its hard edges");

    // The heightmap: satisfies the numeric invariant the header documents.
    expect(num_at(th, "height") == 64.0, "unified: height matches the generator's height_scale");
    const fkyaml::node& sz = th.at("size");
    expect(num_at(sz, "x") == 1024.0 && num_at(sz, "y") == 1024.0, "unified: heightmap size is 1024 x 1024");
    // resolution is corner SAMPLES, not tiles -- it takes height_sub (default 2, a real plane
    // rather than a terraced read) to recover the 1024-tile invariant: (2049 - 1) / 2 == 1024.
    expect(num_at(th, "resolution") == 2049.0,
           "unified: resolution 2049 (1024 tiles * height_sub 2 + 1)");
    expect(num_at(th, "splat_resolution") == 1024.0, "unified: splat_resolution matches world_tiles");
    expect(num_at(th, "chunk_size") == 32.0, "unified: heightmap chunk_tiles matches the tile chunk_size");
    expect(num_at(th, "view_distance") == 320.0, "unified: view_distance matches view_radius x chunk_size");
    expect(num_at(th, "shadow_distance") == 224.0,
           "unified: shadow_distance is tuned below view_distance -- the shadow/scatter budget, "
           "not the numeric invariant, sets this one");
    // Perf guard: nobody should silently re-inflate the shadow/scatter budget the three
    // flavours share (see scene.yaml's SHADOW / SCATTER BUDGET note).
    expect(num_at(th, "shadow_distance") <= 224.0, "unified: heightmap shadow_distance stays tuned down");
    expect(num_at(ts, "shadow_distance") <= 224.0, "unified: soft shadow_distance stays tuned down");
    expect(num_at(tv, "shadow_distance") <= 224.0, "unified: voxel shadow_distance stays tuned down");
    expect(!bool_at(th, "deep_snow", true) && !bool_at(th, "puddles", true),
           "unified: deep_snow and puddles are off -- effects the tile flavours cannot have");
    require(th.contains("layers"), "unified: the heightmap authors layers:");
    std::size_t layer_count = 0;
    for (const auto& ignored : th.at("layers")) { (void)ignored; ++layer_count; }
    expect(layer_count == 8, "unified: eight paint layers, one of the nine reachable kinds dropped");

    // One generator, no target named (so it applies to all three), auto_apply: always (the
    // heightmap mode never auto-applies under the default when_missing).
    const fkyaml::node world = find_root_object(doc, "world");
    require(!world.is_null(), "unified: the world object exists");
    const fkyaml::node gen = find_component(world, "MapGenerator");
    require(!gen.is_null(), "unified: it carries the MapGenerator");
    expect(!gen.contains("target"), "unified: the generator names no target, so it applies to all three");
    expect(str_at(gen, "auto_apply") == "always", "unified: auto_apply is always");
    expect(num_at(gen, "world_tiles") == 1024.0 && num_at(gen, "height_scale") == 64.0,
           "unified: the generator's world_tiles and height_scale match the invariant");

    // The switcher: one per scene, three presets, starting on soft.
    const fkyaml::node ui = find_root_object(doc, "ui");
    require(!ui.is_null(), "unified: the ui object exists");
    const fkyaml::node switcher = find_component(ui, "EnvPresetSwitcher");
    require(!switcher.is_null(), "unified: it carries the EnvPresetSwitcher");
    require(switcher.contains("presets"), "unified: it authors presets");
    std::size_t preset_count = 0;
    for (const auto& ignored : switcher.at("presets")) { (void)ignored; ++preset_count; }
    expect(preset_count == 3, "unified: exactly three presets");
    expect(str_at(switcher, "start") == "soft", "unified: starts on soft");

    // Every terrain's scatter is its own child and names no explicit terrain object -- an
    // explicit name would resolve through find_object(), which finds inactive objects too, and
    // break the "hidden terrains don't scatter" property the active-ancestor gather relies on.
    for (const fkyaml::node* terrain : {&soft, &voxel, &smooth}) {
        const fkyaml::node grass = find_child(*terrain, "grass");
        require(!grass.is_null(), "unified: every terrain has a `grass` scatter child");
        const fkyaml::node ps = find_component(grass, "ParticleSystem");
        require(!ps.is_null(), "unified: ...carrying a ParticleSystem");
        bool names_terrain = ps.contains("terrain") && ps.at("terrain").contains("object");
        expect(!names_terrain, "unified: no scatter names an explicit terrain object");
        // Perf guard: three flavours' worth of scatter adds up fast -- nobody should silently
        // re-inflate density or draw distance back toward the old, "unusable" budget.
        expect(num_at(ps, "max_draw_distance") <= 48.0,
               "unified: grass max_draw_distance stays tuned down");
        require(ps.contains("terrain") && ps.at("terrain").contains("density"),
                "unified: ...carrying a terrain scatter block with density");
        expect(num_at(ps.at("terrain"), "density") <= 0.6,
               "unified: grass density stays tuned down");
    }
}

/**
 * @brief `chunk_budget_ms` reaches the component in `mode: heightmap`.
 *
 * `parse_heightmap_terrain` is the body `register_scene_components()` dispatches to for this
 * mode (terrain_yaml.h's `infer_terrain_mode`), but that registration only happens under the
 * device-taking overload -- unreachable from a device-free unit test (see find_root_object's
 * comment above). Called directly instead, the way this file's other tests read the YAML tree
 * directly: before any device exists, exactly where this parser already runs cleanly.
 */
COOPA_TEST(heightmap_terrain_reads_chunk_budget_ms) {
    static coopa::asset::AssetManager assets;

    fkyaml::node node = fkyaml::node::mapping();
    node["size"] = fkyaml::node(256);
    node["resolution"] = fkyaml::node(33);
    node["chunk_budget_ms"] = fkyaml::node(5.5);

    coopa::scene::SceneObject obj("terrain_chunk_budget_test");
    coopa::scene::SceneLoader::ParseContext ctx;
    ctx.scene_dir = ".";

    toy::world::parse_heightmap_terrain(node, obj, ctx, assets);

    auto* t = obj.get_component<toy::world::HeightmapTerrain>();
    require(t != nullptr, "chunk_budget_ms: the component was added");
    expect(t->chunk_budget_ms == 5.5f, "chunk_budget_ms: the authored value reaches the component, "
                                        "not the 2.0f default");
}
