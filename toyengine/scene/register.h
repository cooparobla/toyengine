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
 * "WaterBody" likewise: toy::water::WaterSystem bakes and publishes its mesh, and its own
 * `mesh_path` source loads through the AssetManager at parse time.
 */

#ifndef TOYENGINE_SCENE_REGISTER_H
#define TOYENGINE_SCENE_REGISTER_H

#include <cctype>
#include <iostream>
#include <stdexcept>

#include <fkYAML/node.hpp>

#include <toyengine/scene/cloth_renderer.h>



namespace toy {
namespace scene {

/** @brief Reads an {x, y, z} YAML mapping, leaving any absent component at `fallback`. */
glm::vec3 parse_vec3(const fkyaml::node& n, const glm::vec3& fallback);

/** @brief Reads an {r, g, b} YAML mapping, leaving any absent channel at `fallback`. */
glm::vec3 parse_rgb(const fkyaml::node& n, const glm::vec3& fallback);

/**
 * @brief Registers the "CameraController", "CharacterController", "SceneLink", "CharacterAnimDriver", "FootIK",
 *        "KinematicMover", "HealthDriver", "FreeMover", "KinematicController" and "Buoyancy" parsers.
 *
 * Call once at startup, before the first SceneLoader::load() that uses them.
 */
void register_scene_components();

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
void register_scene_components(coopa::gfx::core::Device& device,
                                      coopa::gfx::memory::Allocator& allocator,
                                      coopa::asset::AssetManager& assets,
                                      uint32_t frames_in_flight);

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_REGISTER_H
