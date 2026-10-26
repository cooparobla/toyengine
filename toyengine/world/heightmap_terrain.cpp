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

/** @brief `rel` resolved for READING when it exists, else where a save would create it. */
std::string data_path(const coopa::asset::AssetManager& assets, const std::string& rel, const std::string& base_dir) {
    if (rel.empty()) return {};
    const std::string found = assets.source().resolve(rel, base_dir);
    if (std::filesystem::exists(found)) return found;
    if (std::filesystem::path(rel).is_absolute()) return rel;
    return (std::filesystem::path(base_dir) / rel).string();
}

} // namespace

void parse_heightmap_terrain(const fkyaml::node& node, coopa::scene::SceneObject& obj,
                             const coopa::scene::SceneLoader::ParseContext& ctx,
                             coopa::asset::AssetManager& assets) {
    auto* t = obj.add_component<HeightmapTerrain>();
    // One component type, two modes: this parser authors the heightmap one.
    t->mode = TerrainMode::Heightmap;

    HeightmapGeometry& g = t->geometry;
    if (node.contains("size")) {
        const fkyaml::node& s = node.at("size");
        if (s.is_mapping()) {
            g.size.x = get_f(s, "x", g.size.x);
            g.size.y = get_f(s, "y", g.size.y);
        } else {
            g.size = glm::vec2(s.get_value<float>());
        }
    }
    g.height = get_f(node, "height", g.height);
    g.chunk_size = get_f(node, "chunk_size", g.chunk_size);
    g.resolution = get_i(node, "resolution", g.resolution);
    g.splat_resolution = get_i(node, "splat_resolution", g.splat_resolution);
    g = g.sanitized();

    if (node.contains("generate") && node.at("generate").is_mapping()) {
        const fkyaml::node& gn = node.at("generate");
        HeightmapGenerate& p = t->generate;
        p.seed = static_cast<std::uint32_t>(get_i(gn, "seed", static_cast<int>(p.seed)));
        p.octaves = get_i(gn, "octaves", p.octaves);
        p.feature_size = get_f(gn, "feature_size", p.feature_size);
        p.ridges = get_f(gn, "ridges", p.ridges);
        p.base = get_f(gn, "base", p.base);
        p.amplitude = get_f(gn, "amplitude", p.amplitude);
        p.island = get_b(gn, "island", p.island);
    }

    // Default file names come from the object, so two terrains in one scene don't share.
    const std::string stem = "terrain/" + obj.name();
    t->heightmap = get_s(node, "heightmap", stem + "_height.png");
    t->splat0 = get_s(node, "splat0", stem + "_splat0.png");
    t->splat1 = get_s(node, "splat1", stem + "_splat1.png");
    const std::string& base = ctx.base_dir();
    t->files.height = data_path(assets, t->heightmap, base);
    t->files.splat0 = data_path(assets, t->splat0, base);
    t->files.splat1 = data_path(assets, t->splat1, base);

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

    // How its water looks, if it has any -- the same WaterBody keys a tile terrain takes. Both
    // the still and the flowing bodies over a chunk share it: they meet at shared vertices, and
    // two different wave fields would pull those apart (terrain_water_mesh.h).
    if (node.contains("water")) water::parse_water_settings(node.at("water"), t->water_look);

    t->data = HeightmapStore::instance().acquire(t->files, t->geometry, t->generate);
}

} // namespace world
} // namespace toy
