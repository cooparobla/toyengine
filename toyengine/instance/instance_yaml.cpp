#include <toyengine/instance/instance_painter.h>

#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <coopa/scene/scene_loader.h>
#include <gfxcoopa/engine/components/register.h>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace instance {

namespace {

using core::yaml::read;
using coopa::scene::SceneLoader;

/** @brief A `[min, max]`, `{min, max}`, `{x, y}` or scalar range, as every other range key. */
glm::vec2 as_range(const fkyaml::node& n, glm::vec2 def) {
    if (n.is_sequence()) {
        const auto& seq = n.as_seq();
        if (seq.size() >= 2) return glm::vec2(core::yaml::num(seq[0], def.x), core::yaml::num(seq[1], def.y));
        if (seq.size() == 1) {
            const float v = core::yaml::num(seq[0], def.x);
            return glm::vec2(v, v);
        }
        return def;
    }
    if (n.is_mapping()) {
        if (n.contains("min") || n.contains("max")) {
            return glm::vec2(n.contains("min") ? core::yaml::num(n.at("min"), def.x) : def.x,
                             n.contains("max") ? core::yaml::num(n.at("max"), def.y) : def.y);
        }
        if (n.contains("x") || n.contains("y")) {
            return glm::vec2(n.contains("x") ? core::yaml::num(n.at("x"), def.x) : def.x,
                             n.contains("y") ? core::yaml::num(n.at("y"), def.y) : def.y);
        }
        return def;
    }
    const float v = core::yaml::num(n, def.x);
    return glm::vec2(v, v);
}

void parse_prototype(const fkyaml::node& node, InstancePrototype& p, coopa::asset::AssetManager& assets,
                     const SceneLoader::ParseContext& ctx) {
    read(node, "object", p.object);
    read(node, "mesh", p.mesh);
    read(node, "name", p.name);
    if (!p.object.empty() && !p.mesh.empty()) {
        throw std::runtime_error("[InstancePainter] prototype '" + p.label() +
                                 "' sets both object: and mesh: -- it is one or the other");
    }
    if (p.object.empty() && p.mesh.empty()) {
        throw std::runtime_error("[InstancePainter] a prototype needs either object: or mesh:");
    }
    if (node.contains("collision")) {
        const fkyaml::node& c = node.at("collision");
        // `collision: true` is the obvious thing to write, and means "fit one for me".
        if (c.is_boolean()) {
            p.collision = c.get_value<bool>() ? InstanceCollision::Auto : InstanceCollision::None;
        } else {
            std::string name = c.get_value<std::string>();
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (!parse_instance_collision(name, p.collision)) {
                throw std::runtime_error("[InstancePainter] unknown collision '" + name +
                                         "' (none | auto | box | sphere | capsule | mesh)");
            }
        }
    }
    read(node, "collision_size", p.collision_size);
    read(node, "collision_radius", p.collision_radius);
    read(node, "collision_mesh", p.collision_mesh);
    read(node, "physics_material", p.physics_material);
    if (node.contains("scale_range")) p.scale_range = as_range(node.at("scale_range"), p.scale_range);
    if (node.contains("yaw_range")) p.yaw_range = as_range(node.at("yaw_range"), p.yaw_range);
    read(node, "align_to_normal", p.align_to_normal);
    read(node, "sink", p.sink);
    read(node, "draw_distance", p.draw_distance);
    read(node, "fade_start", p.fade_start);
    read(node, "cast_shadows", p.cast_shadows);

    // A `mesh` prototype's proxy is built here, where the AssetManager and the base dir are:
    // resolution (instance_painter.cpp) runs later and has neither.
    if (!p.mesh.empty()) {
        p.own_proxy = std::make_shared<coopa::gfx::engine::components::MeshRenderer>();
        p.own_proxy->set_mesh_path(p.mesh);
        p.own_proxy->set_mesh(assets.load_async<coopa::gfx::engine::data::Mesh>("meshes/" + p.mesh + ".yaml",
                                                                               ctx.base_dir()));
        if (node.contains("material")) {
            coopa::gfx::engine::components::parse_material_value_(node.at("material"), p.own_proxy->material,
                                                                  assets, ctx);
        }
        p.material = p.own_proxy->material;
    }
}

} // namespace

void register_instance_components(coopa::asset::AssetManager& assets) {
    SceneLoader::register_component_parser(
        "InstancePainter",
        [&assets](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* painter = obj.add_component<InstancePainter>();
            read(node, "chunk_size", painter->chunk_size);
            read(node, "collision_radius", painter->collision_radius);
            read(node, "cast_shadows", painter->cast_shadows);
            read(node, "enabled", painter->enabled);

            if (node.contains("prototypes") && node.at("prototypes").is_sequence()) {
                for (const auto& pn : node.at("prototypes").as_seq()) {
                    InstancePrototype p;
                    parse_prototype(pn, p, assets, ctx);
                    painter->prototypes.push_back(std::move(p));
                }
            }

            // `data:` names the painter's .instances file, scene-relative, default
            // instances/<object>.instances. The store hands back the same live records on every
            // re-parse (unsaved edits included), and an empty set when there is no file.
            painter->data_file = node.contains("data") ? node.at("data").get_value<std::string>()
                                                       : "instances/" + obj.name() + ".instances";
            // Strictly beside the scene -- never through the asset search, whose by-name
            // fallback would hand two scenes' same-named painters one shared file.
            const std::string path = std::filesystem::path(painter->data_file).is_absolute()
                                         ? painter->data_file
                                         : (std::filesystem::path(ctx.base_dir()) / painter->data_file)
                                               .lexically_normal().string();
            painter->data = InstanceSetStore::instance().acquire(path);
            painter->data->set_chunk_size(painter->chunk_size);
        });
}

} // namespace instance
} // namespace toy
