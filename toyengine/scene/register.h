/**
 * @file register.h
 * @brief Registers toyengine's own scene components as SceneLoader parsers.
 *
 * Mirrors gfxcoopa/engine/components/register.h's pattern. Most of these components need no
 * dependencies at all, so the plain register_scene_components() overload stays argument-free;
 * ClothRenderer is the exception (it allocates a GPU mesh and publishes it as a runtime asset),
 * so it lives behind the second overload, which captures device/allocator/assets exactly the way
 * register_render_components() captures its own. "Terrain" is there too, for the same reason at
 * one remove: its chunk meshes are built and published at runtime by toy::world::TerrainSystem,
 * and its own atlas texture and side meshes load through the AssetManager at parse time.
 */

#ifndef TOYENGINE_SCENE_REGISTER_H
#define TOYENGINE_SCENE_REGISTER_H

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <fkYAML/node.hpp>

#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/cloth_renderer.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/scene/health_driver.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/kinematic_mover.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

#include <coopa/asset/asset_manager.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/memory/allocator.h>

#include <toyengine/world/terrain_component.h>

namespace toy {
namespace scene {

/** @brief Reads an {x, y, z} YAML mapping, leaving any absent component at `fallback`. */
inline glm::vec3 parse_vec3(const fkyaml::node& n, const glm::vec3& fallback) {
    glm::vec3 v = fallback;
    if (n.contains("x")) v.x = n.at("x").get_value<float>();
    if (n.contains("y")) v.y = n.at("y").get_value<float>();
    if (n.contains("z")) v.z = n.at("z").get_value<float>();
    return v;
}

/**
 * @brief Registers the "CameraController", "KinematicMover" and "HealthDriver" parsers.
 *
 * Call once at startup, before the first SceneLoader::load() that uses them.
 */
inline void register_scene_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("CameraController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cc = obj.add_component<CameraController>();

            if (node.contains("mode")) {
                std::string m = node.at("mode").get_value<std::string>();
                cc->mode = (m == "fly" || m == "Fly") ? CameraControlMode::Fly : CameraControlMode::Orbit;
            }

            if (node.contains("tracker")) {
                cc->tracker = node.at("tracker").get_value<std::string>();
            }
            if (node.contains("target")) {
                cc->target = parse_vec3(node.at("target"), cc->target);
            }
            if (node.contains("target_offset")) {
                cc->target_offset = parse_vec3(node.at("target_offset"), cc->target_offset);
            }
            if (node.contains("follow_smoothing")) {
                cc->follow_smoothing = node.at("follow_smoothing").get_value<float>();
            }
            if (node.contains("movement_smoothing")) {
                cc->movement_smoothing = node.at("movement_smoothing").get_value<float>();
            }

            if (node.contains("distance")) cc->distance = node.at("distance").get_value<float>();
            if (node.contains("yaw_deg"))   cc->yaw_deg   = node.at("yaw_deg").get_value<float>();
            if (node.contains("pitch_deg")) cc->pitch_deg = node.at("pitch_deg").get_value<float>();

            if (node.contains("min_pitch_deg")) cc->min_pitch_deg = node.at("min_pitch_deg").get_value<float>();
            if (node.contains("max_pitch_deg")) cc->max_pitch_deg = node.at("max_pitch_deg").get_value<float>();
            if (node.contains("min_distance"))  cc->min_distance  = node.at("min_distance").get_value<float>();
            if (node.contains("max_distance"))  cc->max_distance  = node.at("max_distance").get_value<float>();

            if (node.contains("mouse_sensitivity")) {
                cc->mouse_sensitivity = node.at("mouse_sensitivity").get_value<float>();
            }
            if (node.contains("invert_x")) cc->invert_x = node.at("invert_x").get_value<bool>();
            if (node.contains("invert_y")) cc->invert_y = node.at("invert_y").get_value<bool>();
            if (node.contains("zoom_speed")) cc->zoom_speed = node.at("zoom_speed").get_value<float>();
            if (node.contains("capture_cursor")) {
                cc->capture_cursor = node.at("capture_cursor").get_value<bool>();
            }

            if (node.contains("auto_rotate_deg_per_sec")) {
                cc->auto_rotate_deg_per_sec = node.at("auto_rotate_deg_per_sec").get_value<float>();
            }
            if (node.contains("move_speed")) {
                cc->move_speed = node.at("move_speed").get_value<float>();
            }
            if (node.contains("look_speed_deg_per_sec")) {
                cc->look_speed_deg_per_sec = node.at("look_speed_deg_per_sec").get_value<float>();
            }
        });

    SceneLoader::register_component_parser("KinematicMover",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* km = obj.add_component<KinematicMover>();

            if (node.contains("mode")) {
                std::string m = node.at("mode").get_value<std::string>();
                if (m == "orbit" || m == "Orbit") km->mode = KinematicMoverMode::Orbit;
                else if (m == "spin" || m == "Spin") km->mode = KinematicMoverMode::Spin;
                else km->mode = KinematicMoverMode::PingPong;
            }
            if (node.contains("axis")) km->axis = parse_vec3(node.at("axis"), km->axis);
            if (node.contains("distance")) km->distance = node.at("distance").get_value<float>();
            if (node.contains("speed")) km->speed = node.at("speed").get_value<float>();
            if (node.contains("orbit_center")) km->orbit_center = parse_vec3(node.at("orbit_center"), km->orbit_center);
            if (node.contains("orbit_radius")) km->orbit_radius = node.at("orbit_radius").get_value<float>();
            if (node.contains("spin_axis")) km->spin_axis = parse_vec3(node.at("spin_axis"), km->spin_axis);
            if (node.contains("spin_speed")) km->spin_speed = node.at("spin_speed").get_value<float>();
        });

    // Demo driver for the world-space UI canvas: owns a coopa::stat::Resource and binds it to
    // a ProgressBar in its own subtree at start(). See toyengine/scene/health_driver.h.
    SceneLoader::register_component_parser("HealthDriver",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* hd = obj.add_component<HealthDriver>();

            if (node.contains("bar_object")) hd->bar_object = node.at("bar_object").get_value<std::string>();
            if (node.contains("max_health")) hd->max_health = node.at("max_health").get_value<float>();
            if (node.contains("start_health")) hd->start_health = node.at("start_health").get_value<float>();
            if (node.contains("damage_per_second")) hd->damage_per_second = node.at("damage_per_second").get_value<float>();
            if (node.contains("regen_per_second")) hd->regen_per_second = node.at("regen_per_second").get_value<float>();
            if (node.contains("turnaround_fraction")) hd->turnaround_fraction = node.at("turnaround_fraction").get_value<float>();
        });

    // Free 3D movement for an object that is not a physics body -- typically an invisible
    // marker a camera tracks and the lens focuses on. Input is pushed in by
    // Engine::drive_free_movers_(), never read here; see the class doc.
    SceneLoader::register_component_parser("FreeMover",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* fm = obj.add_component<FreeMover>();
            if (node.contains("move_speed")) fm->move_speed = node.at("move_speed").get_value<float>();
            if (node.contains("smoothing"))  fm->smoothing  = node.at("smoothing").get_value<float>();
        });

    // Input-driven horizontal motion for a kinematic Rigidbody. The input itself is pushed in by
    // Engine::drive_kinematic_controllers_(), never read here -- see the class doc.
    SceneLoader::register_component_parser("KinematicController",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* kc = obj.add_component<KinematicController>();
            if (node.contains("move_speed")) kc->move_speed = node.at("move_speed").get_value<float>();
            if (node.contains("smoothing")) kc->smoothing = node.at("smoothing").get_value<float>();
            if (node.contains("lock_height")) kc->lock_height = node.at("lock_height").get_value<bool>();
        });
}

/**
 * @brief Registers the argument-free parsers above, plus "ClothRenderer", which needs GPU handles.
 *
 * Call this overload instead of the argument-free one from any app that has a device -- it is a
 * strict superset. Split rather than made the only form so headless tests (and libcoopa-only
 * consumers) can still register the input/behaviour components without a Vulkan device.
 *
 * The captured references must outlive every scene load, exactly like
 * register_render_components()' own captures; Engine clears the whole registry in its destructor
 * via SceneLoader::clear_component_parsers(), while ctx_ is still alive.
 *
 * @param device            Vulkan logical device, for the cloth's dynamic vertex buffers.
 * @param allocator         VMA allocator.
 * @param assets            Asset manager the runtime cloth mesh is published into.
 * @param frames_in_flight  Number of vertex buffers each cloth mesh rings through; pass
 *                          Context::frames_in_flight(). See Mesh::from_arrays().
 */
inline void register_scene_components(coopa::gfx::core::Device& device,
                                      coopa::gfx::memory::Allocator& allocator,
                                      coopa::asset::AssetManager& assets,
                                      uint32_t frames_in_flight) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    register_scene_components();

    // No fields of its own: everything it draws comes from the sibling Cloth and MeshRenderer.
    SceneLoader::register_component_parser("ClothRenderer",
        [&device, &allocator, &assets, frames_in_flight](
            const fkyaml::node&, SceneObject& obj, const SceneLoader::ParseContext&) {
            obj.add_component<ClothRenderer>(device, allocator, assets, frames_in_flight);
        });

    // The streamed tile world (toyengine/world/). The component is configuration plus state;
    // every per-frame decision lives in toy::world::TerrainSystem, which toy::core::Engine
    // installs. Only `assets` is captured -- the chunk meshes this eventually builds are created
    // by that system, which holds the device and allocator itself.
    SceneLoader::register_component_parser("Terrain",
        [&assets](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* terrain = obj.add_component<world::TerrainComponent>();
            world::TerrainParams& params = terrain->params;

            // --- The world to generate ---
            if (node.contains("seed"))      terrain->seed      = node.at("seed").get_value<int>();
            if (node.contains("grid_size")) terrain->grid_size = node.at("grid_size").get_value<int>();
            if (node.contains("sea_level")) terrain->sea_level = node.at("sea_level").get_value<double>();
            if (node.contains("terrain_roughness")) {
                terrain->terrain_roughness = node.at("terrain_roughness").get_value<double>();
            }
            if (node.contains("river_count")) {
                terrain->river_count = node.at("river_count").get_value<int>();
            }

            // --- How it is tiled ---
            if (node.contains("tiles_per_grid_unit")) {
                params.tiles_per_grid_unit = node.at("tiles_per_grid_unit").get_value<std::int32_t>();
            }
            if (node.contains("tile_size"))    params.tile_size    = node.at("tile_size").get_value<float>();
            if (node.contains("height_step"))  params.height_step  = node.at("height_step").get_value<float>();
            if (node.contains("height_scale")) params.height_scale = node.at("height_scale").get_value<float>();
            if (node.contains("chunk_size")) {
                params.chunk_size = node.at("chunk_size").get_value<std::int32_t>();
            }
            if (node.contains("view_radius")) {
                params.view_radius = node.at("view_radius").get_value<std::int32_t>();
            }
            if (node.contains("max_wall_steps")) {
                params.max_wall_steps = node.at("max_wall_steps").get_value<std::int32_t>();
            }
            if (node.contains("soil_depth_steps")) {
                params.soil_depth_steps = node.at("soil_depth_steps").get_value<std::int32_t>();
            }
            if (node.contains("emit_bottom")) {
                params.emit_bottom = node.at("emit_bottom").get_value<bool>();
            }
            if (node.contains("max_chunk_jobs_per_frame")) {
                terrain->max_chunk_jobs_per_frame = node.at("max_chunk_jobs_per_frame").get_value<int>();
            }

            // Shared with the "MeshRenderer" parser rather than reimplemented, so a terrain's
            // atlas gets the same sRGB colour-space declaration and async load path every other
            // textured material in the engine gets -- see gfxcoopa's parse_pbr_material_().
            if (node.contains("material")) {
                coopa::gfx::engine::components::parse_pbr_material_(
                    node.at("material"), terrain->material, assets, ctx);
            }

            // --- The side meshes ---
            // Resolved and load-kicked off here, like SkinnedMeshRenderer's `mesh_path` below,
            // since only the parser has ctx.scene_dir. These are CPU-only SkinnedMeshSource
            // loads (see tile_mesh_library.h for why that type); the component bakes them into
            // its TileMeshLibrary once they land.
            using coopa::gfx::engine::data::SkinnedMeshSource;
            auto load_side = [&assets, &ctx](const std::string& key) {
                return assets.load_async<SkinnedMeshSource>("meshes/" + key + ".yaml", ctx.scene_dir);
            };

            if (node.contains("side_mesh")) {
                terrain->side_mesh = node.at("side_mesh").get_value<std::string>();
            }
            if (!terrain->side_mesh.empty()) terrain->set_side_source(load_side(terrain->side_mesh));

            if (node.contains("sides")) {
                // A face whose name is absent simply inherits the canonical mesh -- which is the
                // point of the indirection: a smoother top is one key here, not a code change.
                static const std::pair<const char*, world::TileFace> k_face_names[] = {
                    {"top", world::TileFace::Top},     {"bottom", world::TileFace::Bottom},
                    {"north", world::TileFace::North}, {"south", world::TileFace::South},
                    {"east", world::TileFace::East},   {"west", world::TileFace::West}};

                const fkyaml::node& sides = node.at("sides");
                for (const auto& [name, face] : k_face_names) {
                    if (!sides.contains(name)) continue;
                    const std::string key = sides.at(name).get_value<std::string>();
                    if (key.empty()) continue;
                    terrain->face_meshes[static_cast<std::size_t>(face)] = key;
                    terrain->set_face_source(face, load_side(key));
                }
            }
        });

    // CPU-skins a bind-pose mesh against animated bone SceneObjects every frame -- see
    // skinned_mesh_renderer.h's file doc for why toyengine does this on the CPU rather than
    // via GPU vertex skinning. `mesh_path` is resolved and load-kicked off here (like
    // register_render_components()'s "MeshRenderer" parser resolves its own), since only the
    // parser has ctx.scene_dir; `bones:` name SceneObject paths in the mesh's joint-index order.
    SceneLoader::register_component_parser("SkinnedMeshRenderer",
        [&device, &allocator, &assets, frames_in_flight](
            const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* smr = obj.add_component<SkinnedMeshRenderer>(device, allocator, assets, frames_in_flight);

            if (node.contains("mesh_path")) {
                std::string mesh_path_key = node.at("mesh_path").get_value<std::string>();
                if (!mesh_path_key.empty()) {
                    std::string virtual_path = "meshes/" + mesh_path_key + ".yaml";
                    smr->set_source(
                        assets.load_async<coopa::gfx::engine::data::SkinnedMeshSource>(virtual_path, ctx.scene_dir));
                }
            }

            if (node.contains("bones")) {
                std::vector<std::string> bones;
                for (const auto& b : node.at("bones")) bones.push_back(b.get_value<std::string>());
                smr->set_bones(std::move(bones));
            }
        });
}

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_REGISTER_H
