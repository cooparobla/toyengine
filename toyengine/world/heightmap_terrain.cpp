#include <toyengine/world/heightmap_terrain.h>

#include <filesystem>
#include <iostream>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/world/heightmap_terrain_system.h>

namespace toy {
namespace world {

HeightmapTerrain::HeightmapTerrain() = default;
HeightmapTerrain::~HeightmapTerrain() = default;

glm::vec3 HeightmapTerrain::origin() const {
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) return glm::vec3(tc->transform().get_world_matrix()[3]);
    }
    return glm::vec3(0.0f);
}

namespace {

float get_f(const fkyaml::node& n, const char* key, float fallback) {
    return n.contains(key) ? n.at(key).get_value<float>() : fallback;
}
int get_i(const fkyaml::node& n, const char* key, int fallback) {
    return n.contains(key) ? n.at(key).get_value<int>() : fallback;
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

void register_heightmap_terrain_component(coopa::asset::AssetManager& assets) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("HeightmapTerrain",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* t = obj.add_component<HeightmapTerrain>();

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

            if (node.contains("layers") && node.at("layers").is_sequence()) {
                for (const auto& ln : node.at("layers")) {
                    if (static_cast<int>(t->layers.size()) >= k_heightmap_layer_count) {
                        std::cerr << "[HeightmapTerrain] '" << obj.name() << "': more than "
                                  << k_heightmap_layer_count << " layers; the rest are ignored\n";
                        break;
                    }
                    HeightmapLayer l;
                    l.name = get_s(ln, "name", "layer" + std::to_string(t->layers.size()));
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
                    t->layer_albedo_paths.push_back(l.albedo.empty() ? std::string() : assets.source().resolve(l.albedo, base));
                    t->layer_normal_paths.push_back(l.normal.empty() ? std::string() : assets.source().resolve(l.normal, base));
                    t->layers.push_back(std::move(l));
                }
            }
            if (t->layers.empty()) {
                // A terrain always has a layer to show: neutral grey-green ground.
                HeightmapLayer l;
                l.name = "ground";
                l.tint = glm::vec3(0.42f, 0.45f, 0.36f);
                t->layers.push_back(l);
                t->layer_albedo_paths.emplace_back();
                t->layer_normal_paths.emplace_back();
            }

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
            t->patch_resolution = std::clamp(get_i(node, "patch_resolution", t->patch_resolution), 1, 64);
            t->layer_texture_size = std::clamp(get_i(node, "layer_texture_size", t->layer_texture_size), 4, 4096);
            t->collision = get_b(node, "collision", t->collision);
            t->physics_material = get_s(node, "physics_material");
            t->deep_snow = get_b(node, "deep_snow", t->deep_snow);
            t->puddles = get_b(node, "puddles", t->puddles);
            t->cast_shadows = get_b(node, "cast_shadows", t->cast_shadows);

            t->data = HeightmapStore::instance().acquire(t->files, t->geometry, t->generate);
        });
}

} // namespace world
} // namespace toy
