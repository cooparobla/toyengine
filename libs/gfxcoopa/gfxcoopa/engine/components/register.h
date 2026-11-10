/**
 * @file register.h
 * @brief Registers gfxcoopa's render components as coopa::scene::SceneLoader parsers.
 *
 * libcoopa's SceneLoader only understands hierarchy plus Transform/Animation —
 * every renderer-specific component (MeshRenderer, Camera, lights, GI/reflection
 * probes) is parsed here instead, keeping the scene system itself free of any
 * gfxcoopa/Vulkan dependency. Call register_render_components() once at startup,
 * before the first SceneLoader::load() that needs these components.
 *
 * MeshRenderer needs GPU handles (Device/Allocator/CommandPool) to upload mesh
 * data during parsing — those are captured by reference in the registered
 * lambdas, which is why this function takes them as parameters instead of a
 * parser receiving them per-call (SceneLoader::ComponentParser's signature has
 * no room for them, deliberately, so the scene system stays decoupled). This
 * means the referenced Device/Allocator/CommandPool must outlive every
 * subsequent SceneLoader::load() call; call
 * coopa::scene::SceneLoader::clear_component_parsers() before destroying them.
 *
 * Mesh and texture loading are routed through a coopa::asset::AssetManager — see
 * gfxcoopa/engine/loaders/{mesh_loader.h,texture_loader.h} for the
 * loaders registered against it. The caller owns the AssetManager (typically
 * for the app's whole lifetime) and must register those two loaders on it
 * before calling register_render_components(); this function only calls
 * assets.load()/load_async(), never registers loaders itself, so a caller
 * can substitute its own loader (e.g. a caml-backed one) if it wants to.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_REGISTER_H
#define GFXCOOPA_ENGINE_COMPONENTS_REGISTER_H

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/asset/asset_manager.h>
#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/engine/loaders/texture_loader.h>
#include <gfxcoopa/types/enums.h>

#include <filesystem>
#include <mutex>
#include <unordered_map>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <gfxcoopa/engine/components/spot_light.h>
#include <gfxcoopa/engine/components/environment_light.h>
#include <gfxcoopa/engine/components/gi_probe_volume.h>
#include <gfxcoopa/engine/components/reflection_probe.h>
#include <gfxcoopa/engine/components/volume.h>
#include <gfxcoopa/engine/components/sdf_renderer.h>
#include <gfxcoopa/engine/components/sdf_shape.h>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @brief Decodes a scene YAML `alpha_mode` string into an AlphaMode.
 *
 * "CUTOUT" is the Unity-facing name for the same alpha-tested behaviour "MASK"/"CLIP" (the
 * glTF names) already select -- all three collapse onto AlphaMode::Mask; see that enum's doc.
 * Any other string (including an absent/misspelled one) falls back to AlphaMode::Opaque, silently
 * -- matches this parser's existing behaviour for every other unrecognised enum-like value.
 *
 * Factored out of parse_pbr_material_() so it's unit-testable without a Device/AssetManager.
 *
 * @param mode The raw `alpha_mode` string from scene YAML.
 * @return The decoded AlphaMode.
 */
inline ToonSpecular parse_toon_specular_(const std::string& mode) {
    if (mode == "hard") {
        return ToonSpecular::Hard;
    } else if (mode == "twostep" || mode == "two_step") {
        return ToonSpecular::TwoStep;
    } else if (mode == "off" || mode == "none") {
        return ToonSpecular::Off;
    }
    return ToonSpecular::Pbr;
}

inline AlphaMode parse_alpha_mode_(const std::string& mode) {
    if (mode == "BLEND") {
        return AlphaMode::Blend;
    } else if (mode == "MASK" || mode == "CLIP" || mode == "CUTOUT") {
        return AlphaMode::Mask;
    }
    return AlphaMode::Opaque;
}

/**
 * @brief Parses a `material:` YAML block into a PBRMaterial, shared by the
 * "MeshRenderer" and "SdfRenderer" parsers below so the two renderer types'
 * material surface can never drift apart (see SdfRenderer's own doc: it
 * reuses PBRMaterial verbatim for exactly this reason).
 */
inline void parse_pbr_material_(const fkyaml::node& mat_node, PBRMaterial& material,
                                coopa::asset::AssetManager& assets,
                                const coopa::scene::SceneLoader::ParseContext& ctx) {
    if (mat_node.contains("albedo")) {
        const auto& alb = mat_node.at("albedo");
        material.albedo = {
            alb.at("r").get_value<float>(),
            alb.at("g").get_value<float>(),
            alb.at("b").get_value<float>()
        };
    }
    if (mat_node.contains("metallic"))  material.metallic  = mat_node.at("metallic").get_value<float>();
    if (mat_node.contains("roughness")) material.roughness = mat_node.at("roughness").get_value<float>();
    if (mat_node.contains("ao"))        material.ao        = mat_node.at("ao").get_value<float>();

    if (mat_node.contains("emissive")) {
        const auto& em = mat_node.at("emissive");
        material.emissive = {
            em.at("r").get_value<float>(),
            em.at("g").get_value<float>(),
            em.at("b").get_value<float>()
        };
    }
    if (mat_node.contains("emissive_strength")) material.emissive_strength = mat_node.at("emissive_strength").get_value<float>();

    if (mat_node.contains("alpha"))        material.alpha        = mat_node.at("alpha").get_value<float>();
    if (mat_node.contains("alpha_cutoff")) material.alpha_cutoff = mat_node.at("alpha_cutoff").get_value<float>();
    if (mat_node.contains("alpha_mode")) {
        material.alpha_mode = parse_alpha_mode_(mat_node.at("alpha_mode").get_value<std::string>());
    }
    if (mat_node.contains("cull_backfaces")) material.cull_backfaces = mat_node.at("cull_backfaces").get_value<bool>();

    // Mesh-only (forward MESH transparent pass); parsed here regardless of renderer type
    // since this function is shared with SdfRenderer, but ignored by every SDF pass -- see
    // PBRMaterial's own doc on refraction/ior/refraction_thickness/refraction_tint.
    if (mat_node.contains("refraction"))           material.refraction           = mat_node.at("refraction").get_value<bool>();
    if (mat_node.contains("ior"))                  material.ior                  = mat_node.at("ior").get_value<float>();
    if (mat_node.contains("refraction_thickness")) material.refraction_thickness = mat_node.at("refraction_thickness").get_value<float>();
    if (mat_node.contains("refraction_tint")) {
        const auto& t = mat_node.at("refraction_tint");
        material.refraction_tint = {
            t.at("r").get_value<float>(),
            t.at("g").get_value<float>(),
            t.at("b").get_value<float>()
        };
    }

    // Optional per-path color-space override, keyed by the same path a texture_* field below
    // names (relative to this material's scene_dir, matching load_async()'s own resolution).
    // Only needed for the rare case of one PNG reused as both a color map and a non-color map
    // across materials -- every other path gets its slot's default (albedo -> sRGB, everything
    // else -> linear) below without needing an entry here. See
    // gfx::loaders::TextureLoader::declare_color_space()'s doc for what happens on conflict.
    std::unordered_map<std::string, ColorSpace> color_space_overrides;
    if (mat_node.contains("texture_color_space")) {
        for (auto item : mat_node.at("texture_color_space").map_items()) {
            std::string path   = item.key().get_value<std::string>();
            std::string cs_str = item.value().get_value<std::string>();
            color_space_overrides[path] = (cs_str == "srgb" || cs_str == "Srgb")
                ? ColorSpace::Srgb : ColorSpace::Linear;
        }
    }
    // nullptr when the caller registered no Texture loader (or a substitute loader without this
    // method) -- declare_*() calls below are then simply skipped, and every texture uploads
    // linear, same as before color-space declaration existed.
    auto* texture_loader = dynamic_cast<loaders::TextureLoader*>(assets.loader<data::Texture>());
    auto declare_color_space = [&](const std::string& path, ColorSpace slot_default) {
        if (!texture_loader) return;
        // declare_color_space() keys on AssetId::from_path() of whatever string it's given, and
        // AssetManager::load_async() below builds ITS AssetId from the *resolved* path (see
        // asset_manager.h's load_async()) -- resolving here first, the same way, is what makes
        // the two agree on which asset this is, rather than declaring a color space for a
        // virtual-path AssetId the loader's finalize_typed() will never look up.
        std::string resolved = assets.source().resolve(path, ctx.base_dir());
        auto it = color_space_overrides.find(path);
        texture_loader->declare_color_space(resolved, it != color_space_overrides.end() ? it->second : slot_default);
    };

    // An empty path means "no map" (the editor's "(none)", or an override clearing an asset's
    // map): drop the handle rather than asking the AssetManager to load "".
    auto load_map = [&](const char* key, std::string& path, ColorSpace space,
                        coopa::asset::AssetHandle<data::Texture>& handle) {
        if (!mat_node.contains(key)) return;
        path = mat_node.at(key).get_value<std::string>();
        if (path.empty()) { handle = {}; return; }
        declare_color_space(path, space);
        handle = assets.load_async<data::Texture>(path, ctx.base_dir());
    };
    load_map("texture_albedo", material.texture_albedo, ColorSpace::Srgb, material.albedo_handle);
    load_map("texture_normal", material.texture_normal, ColorSpace::Linear, material.normal_handle);
    load_map("texture_metallic_roughness", material.texture_metallic_roughness, ColorSpace::Linear,
             material.metallic_roughness_handle);
    load_map("texture_alpha_mask", material.texture_alpha_mask, ColorSpace::Linear, material.alpha_mask_handle);
    load_map("texture_displacement", material.texture_displacement, ColorSpace::Linear, material.displacement_handle);
    if (mat_node.contains("displacement_scale")) {
        const fkyaml::node& d = mat_node.at("displacement_scale");
        material.displacement_scale = d.is_integer() ? static_cast<float>(d.get_value<int64_t>()) : d.get_value<float>();
    }
    if (mat_node.contains("snow") && mat_node.at("snow").is_boolean()) material.snow = mat_node.at("snow").get_value<bool>();
    if (mat_node.contains("wet") && mat_node.at("wet").is_boolean()) material.wet = mat_node.at("wet").get_value<bool>();

    // Toon (cel) shading, layered on this material's PBR inputs -- see PBRMaterial's own
    // toon_* docs for what each one means and gpu_toon_code() for how they reach the GPU.
    // Parsed here for every renderer type, like refraction above, but consumed by the
    // deferred opaque path only.
    if (mat_node.contains("toon_bands")) {
        const fkyaml::node& b = mat_node.at("toon_bands");
        material.toon_bands = b.is_integer() ? static_cast<int>(b.get_value<int64_t>())
                                             : static_cast<int>(b.get_value<float>());
    }
    if (mat_node.contains("toon_specular")) {
        material.toon_specular = parse_toon_specular_(mat_node.at("toon_specular").get_value<std::string>());
    }
    if (mat_node.contains("toon_rim"))      material.toon_rim      = mat_node.at("toon_rim").get_value<float>();
    if (mat_node.contains("toon_softness")) material.toon_softness = mat_node.at("toon_softness").get_value<float>();
    if (mat_node.contains("toon_outline") && mat_node.at("toon_outline").is_boolean()) {
        material.toon_outline = mat_node.at("toon_outline").get_value<bool>();
    }

    // Derived surface shader (see PBRMaterial::shader's doc and the layered-shaders plan's
    // gfx/surface/*.glsl backbones). Registered-name validation happens later, once a
    // SurfaceShaderRegistry is available (register_render_components() itself has no
    // renderer/pipeline context to validate against) -- see ToyRenderPipeline's ctor,
    // which calls SurfaceShaderRegistry::require() for every parsed material before
    // building any pipeline. shader_params is a plain 4-element list, not named keys, to
    // avoid threading the registry's per-shader param-name table into this free function;
    // a scene author cross-references the shader's own doc for what each slot means (see
    // foliage_surface.glsl/water_surface.glsl's file comments).
    if (mat_node.contains("shader")) {
        material.shader = mat_node.at("shader").get_value<std::string>();
    }
    if (mat_node.contains("shader_params")) {
        const auto& p = mat_node.at("shader_params");
        for (size_t i = 0; i < 4 && i < p.size(); ++i) {
            material.shader_params[static_cast<int>(i)] = p[i].get_value<float>();
        }
    }
    // A flat list of up to 8 floats: [0,4) -> shader_params_ext[0], [4,8) -> [1].
    if (mat_node.contains("shader_params_ext")) {
        const auto& p = mat_node.at("shader_params_ext");
        for (size_t i = 0; i < 8 && i < p.size(); ++i) {
            material.shader_params_ext[i / 4][static_cast<int>(i % 4)] = p[i].get_value<float>();
        }
    }
}

/**
 * @brief Resolves a material reference ("materials/brick", or with an explicit .yaml/.caml)
 *        to the parsed material document, through the asset search roots.
 * @throws std::runtime_error if the reference names no existing file.
 */
inline fkyaml::node load_material_document_(const std::string& ref, coopa::asset::AssetManager& assets,
                                            const coopa::scene::SceneLoader::ParseContext& ctx) {
    // A shared material is referenced by many objects (a scene of 500 rocks names
    // `materials/stone` 500 times): resolve + read + parse it once, not per reference. Each use
    // re-checks the file's write time and size, so an edited material is picked up on the next
    // load -- one stat instead of a resolve (~10 stats) and a parse.
    struct Cached {
        std::string file;   // the resolved document (yaml or caml twin)
        std::filesystem::file_time_type mtime{};
        std::uintmax_t size = 0;
        fkyaml::node doc;
    };
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, Cached> cache;
    const std::string key = ref + '\n' + ctx.base_dir();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            std::error_code ec1, ec2;
            const auto mtime = std::filesystem::last_write_time(it->second.file, ec1);
            const auto size = std::filesystem::file_size(it->second.file, ec2);
            if (!ec1 && !ec2 && mtime == it->second.mtime && size == it->second.size) return it->second.doc;
            cache.erase(it);
        }
    }
    std::string path = ref;
    if (!coopa::yaml::is_document_ext(path)) path += ".yaml";
    const std::string resolved = assets.source().resolve(path, ctx.base_dir());
    if (!coopa::yaml::document_exists(resolved)) {
        throw std::runtime_error("[gfxcoopa] Material '" + ref + "' not found (looked for '" + path +
                                 "' in the scene directory and asset roots)");
    }
    Cached entry;
    entry.file = coopa::yaml::resolve_variant(resolved);
    entry.doc = coopa::yaml::load_document(entry.file);
    std::error_code ec1, ec2;
    entry.mtime = std::filesystem::last_write_time(entry.file, ec1);
    entry.size = std::filesystem::file_size(entry.file, ec2);
    fkyaml::node doc = entry.doc;
    if (!ec1 && !ec2) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        cache[key] = std::move(entry);
    }
    return doc;
}

/**
 * @brief Parses a renderer's `material:` value in any of its three forms:
 *
 *   material: materials/brick                       # a shared material asset
 *   material: { base: materials/brick, roughness: 0.3 }   # an asset plus per-object overrides
 *   material: { albedo: { r: 1, g: 0, b: 0 }, ... }        # fully inline
 *
 * A material asset file holds exactly the keys an inline block does. Texture paths inside
 * it resolve like any other asset path (scene directory first, then the asset roots).
 */
inline void parse_material_value_(const fkyaml::node& value, PBRMaterial& material,
                                  coopa::asset::AssetManager& assets,
                                  const coopa::scene::SceneLoader::ParseContext& ctx) {
    if (value.is_string()) {
        parse_pbr_material_(load_material_document_(value.get_value<std::string>(), assets, ctx), material, assets, ctx);
        return;
    }
    if (value.is_mapping() && value.contains("base") && value.at("base").is_string()) {
        parse_pbr_material_(load_material_document_(value.at("base").get_value<std::string>(), assets, ctx),
                            material, assets, ctx);
    }
    if (value.is_mapping()) parse_pbr_material_(value, material, assets, ctx);
}

/// Parses a `{x:, y:, z:}` node into a glm::vec3, leaving components at
/// `fallback`'s when absent -- the same partial-override convention every
/// other vec3 field in this file already follows (DirectionalLight's
/// direction/color, Volume's extent/color, etc.).
inline glm::vec3 parse_vec3_(const fkyaml::node& node, const glm::vec3& fallback) {
    glm::vec3 v = fallback;
    if (node.contains("x")) v.x = node.at("x").get_value<float>();
    if (node.contains("y")) v.y = node.at("y").get_value<float>();
    if (node.contains("z")) v.z = node.at("z").get_value<float>();
    return v;
}

/**
 * @brief Registers parsers for every gfxcoopa render component with SceneLoader.
 *
 * @param device    Vulkan logical device, used for mesh/texture buffer creation.
 * @param allocator VMA allocator, used for mesh/texture memory allocation.
 * @param cmd_pool  Command pool for one-shot mesh/texture upload transfers.
 * @param assets    AssetManager with MeshLoader and TextureLoader already
 *                  registered (see gfxcoopa/engine/loaders/). Must outlive
 *                  every subsequent SceneLoader::load() call, same as
 *                  device/allocator/cmd_pool.
 */
inline void register_render_components(core::Device& device,
                                       memory::Allocator& allocator,
                                       command::CommandPool& cmd_pool,
                                       coopa::asset::AssetManager& assets) {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser("MeshRenderer",
        [&assets](
            const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* mr = obj.add_component<MeshRenderer>();
            std::string mesh_path_key;
            if (node.contains("mesh_path")) {
                mesh_path_key = node.at("mesh_path").get_value<std::string>();
            }
            mr->set_mesh_path(mesh_path_key);

            if (node.contains("affects_reflection_probes"))
                mr->affects_reflection_probes = node.at("affects_reflection_probes").get_value<bool>();
            if (node.contains("lod_bias"))
                mr->lod_bias = node.at("lod_bias").get_value<float>();
            if (node.contains("lods_enabled"))
                mr->lods_enabled = node.at("lods_enabled").get_value<bool>();
            if (node.contains("cast_shadows"))
                mr->cast_shadows = node.at("cast_shadows").get_value<bool>();
            if (node.contains("shadow_max_distance"))
                mr->shadow_max_distance = node.at("shadow_max_distance").get_value<float>();
            // `tessellation: true` with flat `tess_edge_pixels` / `tess_max_factor` /
            // `tess_max_distance` (the editor's spelling), or one block
            // `tessellation: {enabled, edge_pixels, max_factor, max_distance}`.
            {
                auto num = [](const fkyaml::node& n, float def) {
                    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
                    if (n.is_integer()) return static_cast<float>(n.get_value<int64_t>());
                    return def;
                };
                auto& tess = mr->tessellation;
                if (node.contains("tessellation")) {
                    const fkyaml::node& t = node.at("tessellation");
                    if (t.is_boolean()) {
                        tess.enabled = t.get_value<bool>();
                    } else if (t.is_mapping()) {
                        tess.enabled = !t.contains("enabled") || !t.at("enabled").is_boolean() || t.at("enabled").get_value<bool>();
                        if (t.contains("edge_pixels"))  tess.edge_pixels  = num(t.at("edge_pixels"), tess.edge_pixels);
                        if (t.contains("max_factor"))   tess.max_factor   = num(t.at("max_factor"), tess.max_factor);
                        if (t.contains("max_distance")) tess.max_distance = num(t.at("max_distance"), tess.max_distance);
                    }
                }
                if (node.contains("tess_edge_pixels"))  tess.edge_pixels  = num(node.at("tess_edge_pixels"), tess.edge_pixels);
                if (node.contains("tess_max_factor"))   tess.max_factor   = num(node.at("tess_max_factor"), tess.max_factor);
                if (node.contains("tess_max_distance")) tess.max_distance = num(node.at("tess_max_distance"), tess.max_distance);
            }

            if (!mesh_path_key.empty()) {
                // Async: the fkYAML mesh parse (data::Mesh::from_node) is pure CPU decode with
                // no GPU calls, so it runs on a JobEngine worker like the texture loads just
                // below already do -- meshes in a scene decode concurrently instead of
                // serializing at parse time. is_failed() can't be checked here since the
                // decode hasn't necessarily run yet; the caller must poll AssetManager::update()
                // (e.g. via a startup drain loop) and check mr->mesh_handle() then.
                std::string mesh_virtual_path = "meshes/" + mesh_path_key + ".yaml";
                mr->set_mesh(assets.load_async<data::Mesh>(mesh_virtual_path, ctx.base_dir()));
            }

            if (node.contains("material")) {
                parse_material_value_(node.at("material"), mr->material, assets, ctx);
            }
            // Per-slot materials (submeshes): a list by slot index, or a map by slot name.
            if (node.contains("materials")) {
                const auto& mats = node.at("materials");
                if (mats.is_sequence()) {
                    uint32_t slot = 0;
                    for (const auto& m : mats) {
                        PBRMaterial pm = slot == 0 ? mr->material : PBRMaterial{};
                        parse_material_value_(m, pm, assets, ctx);
                        mr->material_for_mut(slot) = pm;
                        ++slot;
                    }
                } else if (mats.is_mapping()) {
                    for (const auto& [k, m] : mats.as_map()) {
                        PBRMaterial pm;
                        parse_material_value_(m, pm, assets, ctx);
                        mr->pending_named_materials.push_back({k.get_value<std::string>(), pm});
                    }
                }
            }
        });

    SceneLoader::register_component_parser("Camera",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* cam = obj.add_component<CameraComponent>();

            if (node.contains("projection")) {
                std::string p_str = node.at("projection").get_value<std::string>();
                cam->type = (p_str == "ORTHO" || p_str == "Orthographic" || p_str == "orthographic")
                            ? CameraType::Orthographic : CameraType::Perspective;
            } else if (node.contains("type")) {
                std::string type_str = node.at("type").get_value<std::string>();
                if (type_str != "Camera") {
                    cam->type = (type_str == "ORTHO" || type_str == "Orthographic" || type_str == "orthographic")
                                ? CameraType::Orthographic : CameraType::Perspective;
                }
            }

            if (node.contains("fov")) cam->fov = node.at("fov").get_value<float>();

            if (node.contains("orthographic_size")) {
                cam->orthographic_size = node.at("orthographic_size").get_value<float>();
            } else if (node.contains("ortho_size")) {
                cam->orthographic_size = node.at("ortho_size").get_value<float>();
            } else if (node.contains("ortho_scale")) {
                cam->set_ortho_scale(node.at("ortho_scale").get_value<float>());
            }

            if (node.contains("near_clip_plane")) {
                cam->clip_start = node.at("near_clip_plane").get_value<float>();
            } else if (node.contains("clip_start")) {
                cam->clip_start = node.at("clip_start").get_value<float>();
            }

            if (node.contains("far_clip_plane")) {
                cam->clip_end = node.at("far_clip_plane").get_value<float>();
            } else if (node.contains("clip_end")) {
                cam->clip_end = node.at("clip_end").get_value<float>();
            }

            if (node.contains("lens"))          cam->lens          = node.at("lens").get_value<float>();
            if (node.contains("sensor_width"))  cam->sensor_width  = node.at("sensor_width").get_value<float>();
            if (node.contains("sensor_height")) cam->sensor_height = node.at("sensor_height").get_value<float>();

            // Per-camera depth-of-field overrides; aperture/focus_distance default to 0 and
            // focus_object to "", all meaning "inherit the render config" (see CameraComponent's
            // own field docs), so omitting them leaves an existing scene byte-for-byte unchanged.
            if (node.contains("aperture"))       cam->aperture       = node.at("aperture").get_value<float>();
            if (node.contains("focus_distance")) cam->focus_distance = node.at("focus_distance").get_value<float>();
            if (node.contains("focus_object"))   cam->focus_object   = node.at("focus_object").get_value<std::string>();
            if (node.contains("motion_blur"))    cam->motion_blur    = node.at("motion_blur").get_value<bool>();

            if (node.contains("main")) {
                cam->is_main = node.at("main").get_value<bool>();
            } else if (node.contains("is_main")) {
                cam->is_main = node.at("is_main").get_value<bool>();
            }
        });

    SceneLoader::register_component_parser("DirectionalLight",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* dl = obj.add_component<DirectionalLightComponent>();

            if (node.contains("direction")) {
                const auto& d = node.at("direction");
                float dx = d.contains("x") ? d.at("x").get_value<float>() : dl->direction.x;
                float dy = d.contains("y") ? d.at("y").get_value<float>() : dl->direction.y;
                float dz = d.contains("z") ? d.at("z").get_value<float>() : dl->direction.z;
                dl->direction = glm::normalize(glm::vec3(dx, dy, dz));
            }
            if (node.contains("color")) {
                const auto& c = node.at("color");
                dl->color.r = c.contains("r") ? c.at("r").get_value<float>() : dl->color.r;
                dl->color.g = c.contains("g") ? c.at("g").get_value<float>() : dl->color.g;
                dl->color.b = c.contains("b") ? c.at("b").get_value<float>() : dl->color.b;
            }
            if (node.contains("intensity")) {
                dl->intensity = node.at("intensity").get_value<float>();
            }
            if (node.contains("cast_shadows")) {
                dl->cast_shadows = node.at("cast_shadows").get_value<bool>();
            }
            if (node.contains("shadow_intensity")) {
                dl->shadow_intensity = node.at("shadow_intensity").get_value<float>();
            }
        });

    SceneLoader::register_component_parser("PointLight",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* pl = obj.add_component<PointLightComponent>();

            if (node.contains("color")) {
                const auto& c = node.at("color");
                pl->color.r = c.contains("r") ? c.at("r").get_value<float>() : pl->color.r;
                pl->color.g = c.contains("g") ? c.at("g").get_value<float>() : pl->color.g;
                pl->color.b = c.contains("b") ? c.at("b").get_value<float>() : pl->color.b;
            }
            if (node.contains("intensity")) pl->intensity = node.at("intensity").get_value<float>();
            if (node.contains("range"))     pl->range     = node.at("range").get_value<float>();
            if (node.contains("cast_shadows")) pl->cast_shadows = node.at("cast_shadows").get_value<bool>();
            if (node.contains("attenuation_constant"))
                pl->attenuation_constant = node.at("attenuation_constant").get_value<float>();
            if (node.contains("attenuation_linear"))
                pl->attenuation_linear = node.at("attenuation_linear").get_value<float>();
            if (node.contains("attenuation_quadratic"))
                pl->attenuation_quadratic = node.at("attenuation_quadratic").get_value<float>();
        });

    SceneLoader::register_component_parser("SpotLight",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* sl = obj.add_component<SpotLightComponent>();

            if (node.contains("color")) {
                const auto& c = node.at("color");
                sl->color.r = c.contains("r") ? c.at("r").get_value<float>() : sl->color.r;
                sl->color.g = c.contains("g") ? c.at("g").get_value<float>() : sl->color.g;
                sl->color.b = c.contains("b") ? c.at("b").get_value<float>() : sl->color.b;
            }
            if (node.contains("direction")) {
                sl->direction = parse_vec3_(node.at("direction"), sl->direction);
            }
            if (node.contains("intensity"))   sl->intensity   = node.at("intensity").get_value<float>();
            if (node.contains("range"))       sl->range       = node.at("range").get_value<float>();
            if (node.contains("inner_angle")) sl->inner_angle = node.at("inner_angle").get_value<float>();
            if (node.contains("outer_angle")) sl->outer_angle = node.at("outer_angle").get_value<float>();
            if (node.contains("cast_shadows")) sl->cast_shadows = node.at("cast_shadows").get_value<bool>();
            if (node.contains("attenuation_constant"))
                sl->attenuation_constant = node.at("attenuation_constant").get_value<float>();
            if (node.contains("attenuation_linear"))
                sl->attenuation_linear = node.at("attenuation_linear").get_value<float>();
            if (node.contains("attenuation_quadratic"))
                sl->attenuation_quadratic = node.at("attenuation_quadratic").get_value<float>();
        });

    SceneLoader::register_component_parser("GiProbeVolume",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* gv = obj.add_component<GiProbeVolumeComponent>();
            if (node.contains("origin")) {
                const auto& o = node.at("origin");
                gv->origin.x = o.contains("x") ? o.at("x").get_value<float>() : gv->origin.x;
                gv->origin.y = o.contains("y") ? o.at("y").get_value<float>() : gv->origin.y;
                gv->origin.z = o.contains("z") ? o.at("z").get_value<float>() : gv->origin.z;
            }
            if (node.contains("extent")) {
                const auto& e = node.at("extent");
                gv->extent.x = e.contains("x") ? e.at("x").get_value<float>() : gv->extent.x;
                gv->extent.y = e.contains("y") ? e.at("y").get_value<float>() : gv->extent.y;
                gv->extent.z = e.contains("z") ? e.at("z").get_value<float>() : gv->extent.z;
            }
            if (node.contains("grid_resolution")) {
                const auto& g = node.at("grid_resolution");
                gv->grid_resolution.x = g.contains("x") ? g.at("x").get_value<int>() : gv->grid_resolution.x;
                gv->grid_resolution.y = g.contains("y") ? g.at("y").get_value<int>() : gv->grid_resolution.y;
                gv->grid_resolution.z = g.contains("z") ? g.at("z").get_value<int>() : gv->grid_resolution.z;
            }
            if (node.contains("update_mode")) {
                std::string mode = node.at("update_mode").get_value<std::string>();
                gv->update_mode = (mode == "dynamic") ? GiUpdateMode::Dynamic : GiUpdateMode::Static;
            }
            if (node.contains("gi_intensity"))
                gv->gi_intensity = node.at("gi_intensity").get_value<float>();
        });
    // Local volume -- the single local-volume concept. Fog is GLOBAL only and
    // config-driven; everything bounded (including a static fog pocket) is one of
    // these, raymarched by VolumetricsPass. `kind` picks the density function.
    //
    // `kind` is read FIRST and applies kind-specific defaults before any other
    // field is parsed: the three kinds want very different values, a C++ struct
    // can only carry one set, and this is what lets a scene declare `kind: haze`
    // plus a transform and get haze. Read it out of order and every volume
    // silently keeps the Wind defaults.
    SceneLoader::register_component_parser("Volume",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* vol = obj.add_component<VolumeComponent>();

            if (node.contains("kind")) {
                std::string k = node.at("kind").get_value<std::string>();
                if      (k == "fog"  || k == "Fog")  vol->kind = VolumeKind::Fog;
                else if (k == "haze" || k == "Haze") vol->kind = VolumeKind::Haze;
                else                                 vol->kind = VolumeKind::Wind;
            }
            switch (vol->kind) {
                case VolumeKind::Fog:
                    // A static pocket: no noise, no advection, and it veils rather
                    // than glows -- a static pocket of mist rather than moving air.
                    vol->speed = 0.0f;  vol->density = 0.5f;  vol->occlusion = 0.8f;
                    vol->sun_amount = 0.2f;  vol->height_falloff = 0.0f;
                    vol->color = glm::vec3(0.8f, 0.85f, 0.9f);
                    break;
                case VolumeKind::Haze:
                    // Large, soft, slow, and CONTINUOUS: coverage 0 and gate_scale 0
                    // together are what keep it a medium rather than isolated blobs.
                    vol->speed = 0.6f;  vol->density = 0.10f;
                    vol->noise_scale = 0.05f;  vol->streak = 1.5f;
                    vol->coverage = 0.0f;      vol->gate_scale = 0.0f;
                    vol->flow_warp = 3.0f;     vol->flow_scale = 0.03f;
                    vol->octaves = 3;          vol->height_falloff = 12.0f;
                    vol->occlusion = 0.6f;     vol->sun_amount = 0.4f;
                    vol->color = glm::vec3(0.72f, 0.75f, 0.82f);
                    break;
                case VolumeKind::Wind:
                    break;  // the struct's own defaults are the tuned Wind values
            }

            if (node.contains("shape")) {
                std::string sh = node.at("shape").get_value<std::string>();
                vol->shape = (sh == "sphere" || sh == "Sphere")
                               ? VolumeShape::Sphere
                               : (sh == "shell" || sh == "Shell") ? VolumeShape::Shell : VolumeShape::Box;
            }
            if (node.contains("extent")) {
                const auto& e = node.at("extent");
                vol->extent.x = e.contains("x") ? e.at("x").get_value<float>() : vol->extent.x;
                vol->extent.y = e.contains("y") ? e.at("y").get_value<float>() : vol->extent.y;
                vol->extent.z = e.contains("z") ? e.at("z").get_value<float>() : vol->extent.z;
            }
            if (node.contains("direction")) {
                const auto& d = node.at("direction");
                vol->direction.x = d.contains("x") ? d.at("x").get_value<float>() : vol->direction.x;
                vol->direction.y = d.contains("y") ? d.at("y").get_value<float>() : vol->direction.y;
                vol->direction.z = d.contains("z") ? d.at("z").get_value<float>() : vol->direction.z;
            }
            if (node.contains("color")) {
                const auto& c = node.at("color");
                vol->color.r = c.contains("r") ? c.at("r").get_value<float>() : vol->color.r;
                vol->color.g = c.contains("g") ? c.at("g").get_value<float>() : vol->color.g;
                vol->color.b = c.contains("b") ? c.at("b").get_value<float>() : vol->color.b;
            }
            if (node.contains("falloff"))        vol->falloff        = node.at("falloff").get_value<float>();
            if (node.contains("speed"))          vol->speed          = node.at("speed").get_value<float>();
            if (node.contains("wind_influence")) vol->wind_influence = node.at("wind_influence").get_value<float>();
            if (node.contains("density"))        vol->density        = node.at("density").get_value<float>();
            if (node.contains("noise_scale"))    vol->noise_scale    = node.at("noise_scale").get_value<float>();
            if (node.contains("streak"))         vol->streak         = node.at("streak").get_value<float>();
            if (node.contains("coverage"))       vol->coverage       = node.at("coverage").get_value<float>();
            if (node.contains("sharpness"))      vol->sharpness      = node.at("sharpness").get_value<float>();
            if (node.contains("gate_scale"))     vol->gate_scale     = node.at("gate_scale").get_value<float>();
            if (node.contains("flow_warp"))      vol->flow_warp      = node.at("flow_warp").get_value<float>();
            if (node.contains("flow_scale"))     vol->flow_scale     = node.at("flow_scale").get_value<float>();
            if (node.contains("octaves"))        vol->octaves        = node.at("octaves").get_value<int>();
            if (node.contains("detail_gain"))    vol->detail_gain    = node.at("detail_gain").get_value<float>();
            if (node.contains("height_base"))    vol->height_base    = node.at("height_base").get_value<float>();
            if (node.contains("height_falloff")) vol->height_falloff = node.at("height_falloff").get_value<float>();
            if (node.contains("occlusion"))      vol->occlusion      = node.at("occlusion").get_value<float>();
            if (node.contains("sun_amount"))     vol->sun_amount     = node.at("sun_amount").get_value<float>();
        });

    SceneLoader::register_component_parser("SdfRenderer",
        [&assets](
            const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* sr = obj.add_component<SdfRenderer>();

            if (node.contains("bounds_center"))
                sr->bounds_center = parse_vec3_(node.at("bounds_center"), sr->bounds_center);
            if (node.contains("bounds_extent"))
                sr->bounds_extent = parse_vec3_(node.at("bounds_extent"), sr->bounds_extent);

            if (node.contains("cast_shadows"))
                sr->cast_shadows = node.at("cast_shadows").get_value<bool>();
            if (node.contains("affects_reflection_probes"))
                sr->affects_reflection_probes = node.at("affects_reflection_probes").get_value<bool>();
            if (node.contains("max_steps"))
                sr->max_steps = node.at("max_steps").get_value<int>();
            if (node.contains("surface_epsilon"))
                sr->surface_epsilon = node.at("surface_epsilon").get_value<float>();
            if (node.contains("normal_epsilon"))
                sr->normal_epsilon = node.at("normal_epsilon").get_value<float>();
            if (node.contains("smoothing"))
                sr->smoothing = node.at("smoothing").get_value<float>();

            if (node.contains("material")) {
                parse_material_value_(node.at("material"), sr->material, assets, ctx);
            }
        });

    SceneLoader::register_component_parser("SdfShape",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* shape = obj.add_component<SdfShape>();

            if (node.contains("shape")) {
                std::string s = node.at("shape").get_value<std::string>();
                if (s == "Box" || s == "box") {
                    shape->type = SdfShapeType::Box;
                } else if (s == "Plane" || s == "plane") {
                    shape->type = SdfShapeType::Plane;
                } else {
                    shape->type = SdfShapeType::Sphere;
                }
            }
            if (node.contains("params"))
                shape->params = parse_vec3_(node.at("params"), shape->params);
            if (node.contains("rounding"))
                shape->rounding = node.at("rounding").get_value<float>();
            if (node.contains("op")) {
                std::string o = node.at("op").get_value<std::string>();
                if (o == "Subtract" || o == "subtract") {
                    shape->op = SdfOperation::Subtract;
                } else if (o == "Intersect" || o == "intersect") {
                    shape->op = SdfOperation::Intersect;
                } else {
                    shape->op = SdfOperation::Union;
                }
            }
            if (node.contains("blend"))
                shape->blend = node.at("blend").get_value<float>();
        });

    SceneLoader::register_component_parser("ReflectionProbe",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* rp = obj.add_component<ReflectionProbeComponent>();
            if (node.contains("box_extent")) {
                const auto& e = node.at("box_extent");
                rp->box_extent.x = e.contains("x") ? e.at("x").get_value<float>() : rp->box_extent.x;
                rp->box_extent.y = e.contains("y") ? e.at("y").get_value<float>() : rp->box_extent.y;
                rp->box_extent.z = e.contains("z") ? e.at("z").get_value<float>() : rp->box_extent.z;
            }
            if (node.contains("resolution"))
                rp->resolution = static_cast<uint32_t>(node.at("resolution").get_value<int>());
            if (node.contains("blend_distance"))
                rp->blend_distance = node.at("blend_distance").get_value<float>();
            if (node.contains("importance"))
                rp->importance = node.at("importance").get_value<int>();
            if (node.contains("intensity"))
                rp->intensity = node.at("intensity").get_value<float>();
        });

    SceneLoader::register_component_parser("EnvironmentLight",
        [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* el = obj.add_component<EnvironmentLightComponent>();
            if (node.contains("sky_color")) {
                const auto& c = node.at("sky_color");
                el->sky_color.r = c.contains("r") ? c.at("r").get_value<float>() : el->sky_color.r;
                el->sky_color.g = c.contains("g") ? c.at("g").get_value<float>() : el->sky_color.g;
                el->sky_color.b = c.contains("b") ? c.at("b").get_value<float>() : el->sky_color.b;
            }
            if (node.contains("ground_color")) {
                const auto& c = node.at("ground_color");
                el->ground_color.r = c.contains("r") ? c.at("r").get_value<float>() : el->ground_color.r;
                el->ground_color.g = c.contains("g") ? c.at("g").get_value<float>() : el->ground_color.g;
                el->ground_color.b = c.contains("b") ? c.at("b").get_value<float>() : el->ground_color.b;
            }
            if (node.contains("sky_intensity"))
                el->sky_intensity = node.at("sky_intensity").get_value<float>();
            if (node.contains("hdri_path"))
                el->hdri_path = node.at("hdri_path").get_value<std::string>();
        });
}

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_REGISTER_H
