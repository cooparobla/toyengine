/**
 * @file particle_yaml.h
 * @brief Registers the "ParticleSystem" and "LightFlicker" scene parsers.
 *
 * Keys are flat (one per setting, grouped by prefix-free module names) so the editor's
 * inspector can describe each with an ordinary field; see the README for the full list.
 * Value forms accepted everywhere a range is expected (Unity's "constant" or "random between
 * two constants"):
 *
 * @code
 * start_size: 0.4                 # constant
 * start_size: [0.3, 0.6]          # random in [min, max]
 * start_size: { min: 0.3, max: 0.6 }
 * start_size: { x: 0.3, y: 0.6 }  # the editor's Vec2 spelling
 * @endcode
 *
 * Curves and gradients are key lists over normalized life:
 *
 * @code
 * size_over_life:  [{t: 0, value: 0.3}, {t: 0.2, value: 1.0}, {t: 1, value: 1.6}]
 * color_over_life: [{t: 0, color: {r: 1, g: 0.9, b: 0.5, a: 1}}, {t: 1, color: {r: 0.4, g: 0.05, b: 0, a: 0}}]
 * @endcode
 */

#ifndef TOYENGINE_PARTICLES_PARTICLE_YAML_H
#define TOYENGINE_PARTICLES_PARTICLE_YAML_H

#include <cctype>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/yaml/document.h>

#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/particles/light_flicker.h>
#include <toyengine/particles/particle_system.h>

namespace toy {
namespace particles {

namespace yaml_detail {

std::string lower(std::string s);

float num(const fkyaml::node& n);

/** @brief A Range from a scalar, [min, max], {min, max} or {x, y}. */
Range range(const fkyaml::node& n, Range fallback = {});

glm::vec3 vec3(const fkyaml::node& n, glm::vec3 v);

glm::vec4 color(const fkyaml::node& n, glm::vec4 c);

/** @brief [{t, value}] / [[t, v]] / a constant (one flat key). */
std::vector<glm::vec2> curve_keys(const fkyaml::node& n);

/** @brief [{t, color: {r,g,b,a}}] or [{t, r, g, b, a}]. */
std::vector<Gradient::Key> gradient_keys(const fkyaml::node& n);

template <typename E>
E enum_of(const std::string& value, std::initializer_list<std::pair<const char*, E>> names, const char* key) {
    const std::string v = lower(value);
    for (const auto& [name, e] : names) {
        if (v == name) return e;
    }
    std::string list;
    for (const auto& [name, e] : names) { (void)e; list += list.empty() ? name : std::string(" | ") + name; }
    throw std::runtime_error(std::string("[ParticleSystem] unknown ") + key + " '" + value + "' (" + list + ")");
}

} // namespace yaml_detail

/**
 * @brief Fills `ps->settings` from a ParticleSystem YAML block. Separate from the parser so the
 *        editor and tests can parse a node without a SceneLoader.
 */
void parse_particle_settings(const fkyaml::node& node, ParticleSettings& s);

/**
 * @brief The sampling surface of meshes/<key>.yaml, built once and shared by every system that
 *        emits from it (a weak cache: freed with the last scene that used it).
 *
 * Read straight from the file rather than through the AssetManager, which caches ONE asset type
 * per resolved path: the same mesh is usually also a MeshRenderer's GPU mesh (Blender's emitter
 * is the object's own mesh), and asking the manager for it a second time as CPU data is refused.
 * coopa::yaml::load_document() decodes .caml too, so a packaged project behaves the same.
 */
std::shared_ptr<const MeshSurface> load_mesh_surface(coopa::asset::AssetManager& assets, const std::string& key,
                                                            const std::string& base_dir);

/**
 * @brief Registers "ParticleSystem" and "LightFlicker". `assets` loads the emitter mesh (CPU
 *        side), the render mesh (mesh mode) and the sprite texture; it must outlive every
 *        SceneLoader::load() that uses these parsers, like every other registration.
 */
void register_particle_components(coopa::asset::AssetManager& assets);

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_PARTICLE_YAML_H
