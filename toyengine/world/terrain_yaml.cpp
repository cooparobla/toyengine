#include <toyengine/world/terrain_yaml.h>

#include <algorithm>
#include <iostream>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/core/yaml_read.h>
#include <toyengine/world/terrain_component.h>

namespace toy {
namespace world {
namespace {

float get_f(const fkyaml::node& n, const char* key, float fallback) {
    return n.contains(key) ? core::yaml::num(n.at(key), fallback) : fallback;
}

bool get_b(const fkyaml::node& n, const char* key, bool fallback) {
    bool v = fallback;
    core::yaml::read(n, key, v);
    return v;
}

std::string get_s(const fkyaml::node& n, const char* key, const std::string& fallback = {}) {
    std::string v = fallback;
    core::yaml::read(n, key, v);
    return v;
}

} // namespace

TerrainMode infer_terrain_mode(const fkyaml::node& node) {
    if (node.contains("mode")) {
        std::string m;
        core::yaml::read(node, "mode", m);
        if (!m.empty()) return terrain_mode_from_name(m);
    }
    for (const char* key : {"tile_set", "tiles", "generator", "world_tiles", "tiles_per_grid_unit",
                            "tile_size", "height_step", "height_scale", "face_tiles", "planet"}) {
        if (node.contains(key)) return TerrainMode::Tiles;
    }
    for (const char* key : {"heightmap", "splat0", "splat1", "resolution", "splat_resolution", "size",
                            "generate", "tessellation", "patch_resolution"}) {
        if (node.contains(key)) return TerrainMode::Heightmap;
    }
    return TerrainMode::Tiles;
}

void parse_terrain_layers(const fkyaml::node& node, const std::string& base,
                          coopa::asset::AssetManager& assets, TerrainComponent& terrain) {
    const bool heightmap = terrain.mode == TerrainMode::Heightmap;
    const int cap = heightmap ? k_heightmap_layer_count : k_terrain_layer_count;
    const char* who = heightmap ? "HeightmapTerrain" : "TileTerrain";
    const std::string name = terrain.owner != nullptr ? terrain.owner->name() : std::string();

    if (node.contains("layers") && node.at("layers").is_sequence()) {
        for (const auto& ln : node.at("layers")) {
            if (static_cast<int>(terrain.layers.size()) >= cap) {
                std::cerr << "[" << who << "] '" << name << "': more than " << cap
                          << " layers; the rest are ignored\n";
                break;
            }
            TerrainLayer l;
            l.name = get_s(ln, "name", "layer" + std::to_string(terrain.layers.size()));
            l.albedo = get_s(ln, "albedo");
            l.normal = get_s(ln, "normal");
            if (ln.contains("tint") && ln.at("tint").is_mapping()) {
                const fkyaml::node& c = ln.at("tint");
                l.tint = glm::vec3(get_f(c, "r", 1.0f), get_f(c, "g", 1.0f), get_f(c, "b", 1.0f));
            }
            l.tiling = std::max(0.01f, get_f(ln, "tiling", l.tiling));
            l.roughness = get_f(ln, "roughness", l.roughness);
            l.metallic = get_f(ln, "metallic", l.metallic);
            l.normal_strength = get_f(ln, "normal_strength", l.normal_strength);
            l.triplanar = get_b(ln, "triplanar", l.triplanar);
            l.triplanar_sharpness = get_f(ln, "triplanar_sharpness", l.triplanar_sharpness);
            // A layer may take its colour and roughness from the palette atlas and draw its
            // detail procedurally instead of sampling `albedo`, which is what the built-in tile
            // layers do.
            if (ln.contains("atlas_cell")) {
                l.atlas_cell = static_cast<int>(core::yaml::num(ln.at("atlas_cell"), -1.0));
            }
            l.procedural = get_b(ln, "procedural", l.albedo.empty() && l.atlas_cell >= 0);
            terrain.layer_albedo_paths.push_back(l.albedo.empty() ? std::string()
                                                                  : assets.source().resolve(l.albedo, base));
            terrain.layer_normal_paths.push_back(l.normal.empty() ? std::string()
                                                                  : assets.source().resolve(l.normal, base));
            terrain.layers.push_back(std::move(l));
        }
    }

    if (terrain.layers.empty() && heightmap) {
        // A smooth surface always has a layer to show, and no palette behind it: neutral ground.
        TerrainLayer l;
        l.name = "ground";
        l.tint = glm::vec3(0.42f, 0.45f, 0.36f);
        terrain.layers.push_back(l);
        terrain.layer_albedo_paths.emplace_back();
        terrain.layer_normal_paths.emplace_back();
    }
    // A Tiles terrain is left empty on purpose: that is how it asks for the built-in set.
}

} // namespace world
} // namespace toy
