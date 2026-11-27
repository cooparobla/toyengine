#include <toyengine/world/terrain_yaml.h>

#include <algorithm>
#include <iostream>

#include <coopa/asset/asset_manager.h>
#include <coopa/yaml/document.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/world/tile_types.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/io/yaml_read.h>
#include <toyengine/world/terrain_component.h>

namespace toy {
namespace world {

namespace {
TerrainParseHook& hook_slot() {
    static TerrainParseHook hook;
    return hook;
}
}  // namespace

void set_terrain_parse_hook(TerrainParseHook hook) { hook_slot() = std::move(hook); }

const TerrainParseHook& terrain_parse_hook() { return hook_slot(); }

namespace {

float get_f(const fkyaml::node& n, const char* key, float fallback) {
    return n.contains(key) ? io::yaml::num(n.at(key), fallback) : fallback;
}

bool get_b(const fkyaml::node& n, const char* key, bool fallback) {
    bool v = fallback;
    io::yaml::read(n, key, v);
    return v;
}

std::string get_s(const fkyaml::node& n, const char* key, const std::string& fallback = {}) {
    std::string v = fallback;
    io::yaml::read(n, key, v);
    return v;
}

} // namespace

void apply_tile_material(TerrainComponent& terrain, const fkyaml::node& authored,
                         coopa::asset::AssetManager& assets,
                         const coopa::scene::SceneLoader::ParseContext& ctx) {
    fkyaml::node material = authored;
    if (material.is_mapping() && !material.contains("texture_albedo")) {
        material["texture_albedo"] = fkyaml::node(std::string("textures/terrain_atlas.png"));
        if (!material.contains("texture_metallic_roughness")) {
            material["texture_metallic_roughness"] = fkyaml::node(std::string("textures/terrain_atlas_mr.png"));
        }
    }
    coopa::gfx::engine::components::parse_material_value_(material, terrain.material, assets, ctx);
}

void apply_default_tile_material(TerrainComponent& terrain, coopa::asset::AssetManager& assets,
                                 const std::string& base_dir) {
    coopa::scene::SceneLoader::ParseContext ctx;
    ctx.scene_dir = base_dir;
    apply_tile_material(terrain, fkyaml::node::mapping(), assets, ctx);
}

int terrain_style_named(const TerrainComponent& terrain, const std::string& name) {
    for (std::size_t i = 0; i < terrain.styles.size(); ++i) {
        if (terrain.styles[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int add_terrain_tile_set(TerrainComponent& terrain, coopa::asset::AssetManager& assets,
                         const std::string& base_dir, const std::string& name,
                         const std::string& value) {
    using coopa::gfx::engine::data::SkinnedMeshSource;
    const auto load_piece = [&assets, &base_dir](const std::string& key) {
        return assets.load_async<SkinnedMeshSource>("meshes/" + key + ".yaml", base_dir);
    };

    TerrainComponent::StyleEntry entry;
    entry.name = name;
    entry.prefix = value;
    const std::size_t index = terrain.styles.size();

    if (value.rfind("objects/", 0) == 0) {
        std::string rel = value;
        if (rel.size() < 5 || rel.compare(rel.size() - 5, 5, ".yaml") != 0) rel += ".yaml";
        try {
            const fkyaml::node doc = coopa::yaml::load_document(assets.source().resolve(rel, base_dir));
            const fkyaml::node& obj = doc.at("object");
            if (obj.contains("round_kind_corners")) {
                entry.round_kind_corners = obj.at("round_kind_corners").get_value<bool>();
            }
            if (obj.contains("children") && obj.at("children").is_sequence()) {
                for (const auto& child : obj.at("children")) {
                    if (!child.contains("name") || !child.contains("components")) continue;
                    const std::string child_name = child.at("name").get_value<std::string>();
                    for (std::size_t p = 0; p < k_tile_piece_count; ++p) {
                        const auto piece = static_cast<TilePiece>(p);
                        if (child_name != tile_piece_name(piece)) continue;
                        for (const auto& comp : child.at("components")) {
                            if (!comp.contains("type") || !comp.contains("mesh_path") ||
                                comp.at("type").get_value<std::string>() != "MeshRenderer") continue;
                            terrain.set_style_piece_source(
                                index, piece, load_piece(comp.at("mesh_path").get_value<std::string>()));
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[TileTerrain] tile set '" << value << "' could not be read: " << e.what() << "\n";
        }
    } else {
        for (std::size_t p = 0; p < k_tile_piece_count; ++p) {
            const auto piece = static_cast<TilePiece>(p);
            terrain.set_style_piece_source(index, piece,
                                           load_piece(value + "_" + std::string(tile_piece_name(piece))));
        }
    }

    terrain.styles.push_back(std::move(entry));
    return static_cast<int>(index);
}

TerrainMode infer_terrain_mode(const fkyaml::node& node) {
    if (node.contains("mode")) {
        std::string m;
        io::yaml::read(node, "mode", m);
        if (!m.empty()) return terrain_mode_from_name(m);
    }
    for (const char* key : {"tile_set", "tiles", "generator", "world_tiles", "tiles_per_grid_unit",
                            "tile_size", "height_step", "height_fine", "height_fine_tilt", "height_scale",
                            "face_tiles"}) {
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
                l.atlas_cell = static_cast<int>(io::yaml::num(ln.at("atlas_cell"), -1.0));
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
