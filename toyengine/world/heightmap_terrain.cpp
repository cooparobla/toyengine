#include <toyengine/world/heightmap_terrain.h>
#include <toyengine/core/yaml_read.h>

#include <filesystem>
#include <iostream>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/world/heightmap_terrain_system.h>
#include <toyengine/water/water_yaml.h>
#include <toyengine/world/terrain_yaml.h>

namespace toy {
namespace world {

namespace {

// Value-returning reads (fallback when the key is absent) -- the counterpart to
// yaml_read.h's present-if-set read(), built on its int-tolerant num() so `x: 0` parses.
float get_f(const fkyaml::node& n, const char* key, float fallback) {
    return n.contains(key) ? core::yaml::num(n.at(key), fallback) : fallback;
}
int get_i(const fkyaml::node& n, const char* key, int fallback) {
    int v = fallback; core::yaml::read(n, key, v); return v;
}
bool get_b(const fkyaml::node& n, const char* key, bool fallback) {
    return n.contains(key) ? n.at(key).get_value<bool>() : fallback;
}
std::string get_s(const fkyaml::node& n, const char* key, const std::string& fallback = {}) {
    return n.contains(key) ? n.at(key).get_value<std::string>() : fallback;
}

} // namespace

void parse_heightmap_terrain(const fkyaml::node& node, coopa::scene::SceneObject& obj,
                             const coopa::scene::SceneLoader::ParseContext& ctx,
                             coopa::asset::AssetManager& assets) {
    auto* t = obj.add_component<HeightmapTerrain>();
    // One component type, two modes: this parser authors the heightmap one.
    t->mode = TerrainMode::Heightmap;

    if (node.contains("size")) {
        const fkyaml::node& s = node.at("size");
        if (s.is_mapping()) {
            t->heightmap_size.x = get_f(s, "x", t->heightmap_size.x);
            t->heightmap_size.y = get_f(s, "y", t->heightmap_size.y);
        } else {
            t->heightmap_size = glm::vec2(s.get_value<float>());
        }
    }
    t->heightmap_height = get_f(node, "height", t->heightmap_height);
    t->heightmap_chunk_size = get_f(node, "chunk_size", t->heightmap_chunk_size);
    t->heightmap_resolution = get_i(node, "resolution", t->heightmap_resolution);
    t->heightmap_splat_resolution = get_i(node, "splat_resolution", t->heightmap_splat_resolution);
    // Height samples per TILE edge: 1 is a terraced read (no height plane at all), the default 2
    // is what every scene authored before this key matches. Raising it is the "internal grid"
    // Coopa wanted a hand-sculpted smooth terrain to be able to have.
    t->height_sub = std::max(1, get_i(node, "height_sub", t->height_sub));

    if (node.contains("generate")) {
        std::cerr << "[HeightmapTerrain] '" << obj.name() << "': `generate:` (a procedural start "
                     "when the file is missing) is gone -- a fresh terrain now starts flat; use a "
                     "MapGenerator for procedural content, or sculpt/import instead\n";
    }

    // The `.terra` file, as both modes now share it; default beside the scene, named for the
    // object. Strictly beside the scene -- never through the asset search, whose by-name
    // fallback would hand two scenes' same-named terrains one shared file (or, for a file that
    // does not exist yet, some other root entirely) -- see register.cpp's Tiles-mode parser,
    // which this mirrors exactly.
    const std::string stem = "terrain/" + obj.name() + ".terra";
    const std::string authored = get_s(node, "data", stem);
    t->data_file = std::filesystem::path(authored).is_absolute()
                       ? authored
                       : (std::filesystem::path(ctx.base_dir()) / authored).lexically_normal().string();

    // A one-shot PNG import (terra_image.h), as privileged as a mapcoopa bake and no more: it
    // only ever fires on a field that has never been baked/imported/sculpted (sync_field()'s
    // needs_bake() check) -- never through the asset search, for the same reason data_file above
    // isn't, and strictly beside the scene even for a file that doesn't exist yet.
    const auto resolve_import = [&](const char* key) -> std::string {
        if (!node.contains("import")) return {};
        const fkyaml::node& imp = node.at("import");
        const std::string authored_path = get_s(imp, key);
        if (authored_path.empty()) return {};
        return std::filesystem::path(authored_path).is_absolute()
                   ? authored_path
                   : (std::filesystem::path(ctx.base_dir()) / authored_path).lexically_normal().string();
    };
    t->import_height_path = resolve_import("height");
    t->import_layer0_path = resolve_import("layer0");
    t->import_layer1_path = resolve_import("layer1");

    parse_terrain_layers(node, ctx.base_dir(), assets, *t);

    if (node.contains("tessellation")) {
        const fkyaml::node& tn = node.at("tessellation");
        if (tn.is_mapping()) {
            t->tessellation.enabled = get_b(tn, "enabled", true);
            t->tessellation.edge_pixels = get_f(tn, "edge_pixels", t->tessellation.edge_pixels);
            t->tessellation.max_factor = get_f(tn, "max_factor", t->tessellation.max_factor);
            t->tessellation.max_distance = get_f(tn, "max_distance", t->tessellation.max_distance);
        } else {
            t->tessellation.enabled = tn.get_value<bool>();
        }
    }
    t->patch_resolution = std::clamp(get_i(node, "patch_resolution", t->patch_resolution), 0, 64);
    // `view_distance: auto` (or absent): the main camera's far clip; a number caps it.
    if (node.contains("view_distance")) {
        const fkyaml::node& v = node.at("view_distance");
        if (v.is_string()) t->view_distance = -1.0f;   // "auto"
        else t->view_distance = get_f(node, "view_distance", -1.0f);
        if (t->view_distance <= 0.0f) t->view_distance = -1.0f;
    }
    t->shadow_distance = std::max(0.0f, get_f(node, "shadow_distance", t->shadow_distance));
    t->layer_texture_size = std::clamp(get_i(node, "layer_texture_size", t->layer_texture_size), 4, 4096);
    t->collision = get_b(node, "collision", t->collision);
    t->physics_material = get_s(node, "physics_material");
    t->deep_snow = get_b(node, "deep_snow", t->deep_snow);
    t->puddles = get_b(node, "puddles", t->puddles);
    t->cast_shadows = get_b(node, "cast_shadows", t->cast_shadows);
    // Owner-thread milliseconds a frame for chunk spawning, water rebuilds and a freshly applied
    // map's water sweep (HeightmapTerrainSystem::stream_chunks_() / refresh_water_()).
    t->chunk_budget_ms = get_f(node, "chunk_budget_ms", t->chunk_budget_ms);

    // `lods:` belongs to Tiles mode (parsed in register.cpp's parse_tile_terrain) -- a heightmap
    // chunk draws one shared patch mesh with lods_enabled = false, so a mesh LOD table has nothing
    // to attach to. Warn rather than silently drop it.
    if (node.contains("lods")) {
        std::cerr << "[HeightmapTerrain] '" << obj.name() << "': `lods:` has no effect in heightmap "
                     "mode -- detail comes from `tessellation:` instead\n";
    }

    // How its water looks, if it has any -- the same WaterBody keys a tile terrain takes. Both
    // the still and the flowing bodies over a chunk share it: they meet at shared vertices, and
    // two different wave fields would pull those apart (terrain_water_mesh.h).
    if (node.contains("water")) water::parse_water_settings(node.at("water"), t->water_look);

    // The shore: how far the bed is graded under the water (grade_water_shore(), terrain_water.h)
    // and how far the surface's apron runs past the wet tiles, buried under the bank
    // (build_smooth_chunk_water(), terrain_water_mesh.h). The Tiles-mode equivalents --
    // water_inset / river_inset -- have no meaning here, where the shoreline is wherever the
    // ground crosses the water's level rather than a terrace to inset from.
    world::TerrainParams& params = t->params;
    params.shore_clearance = get_f(node, "shore_clearance", params.shore_clearance);
    params.shore_grade_depth = get_f(node, "shore_grade_depth", params.shore_grade_depth);
    params.shore_grade_band = get_f(node, "shore_grade_band", params.shore_grade_band);
    params.water_skirt_tiles = get_f(node, "water_skirt_tiles", params.water_skirt_tiles);
    // sync_field()'s geometry needs a height_step even though nothing here is terraced -- it
    // sizes the `steps` cache's quantum, read only if this same data is ever meshed terraced.
    params.height_step = get_f(node, "height_step", params.height_step);

    t->sync_field();
}

} // namespace world
} // namespace toy
