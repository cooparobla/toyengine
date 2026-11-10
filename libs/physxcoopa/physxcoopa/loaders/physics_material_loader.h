/**
 * @file physics_material_loader.h
 * @brief coopa::asset loader for dynamics::PhysicsMaterial -- decodes a small YAML document
 *        (friction/restitution/combine modes) into a shareable, AssetManager-owned material.
 *        Unity's PhysicMaterial asset, made loadable from `physics_materials/<name>.yaml`.
 */

#ifndef PHYSXCOOPA_LOADERS_PHYSICS_MATERIAL_LOADER_H
#define PHYSXCOOPA_LOADERS_PHYSICS_MATERIAL_LOADER_H

#include <physxcoopa/dynamics/physics_material.h>

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_id.h>

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace coopa {
namespace physx {
namespace loaders {

namespace detail {

/** @brief Parses a CombineMode name (case-sensitive, matching the enum's spelling). Unknown or
 *         absent values fall back to `fallback` rather than throwing -- a typo'd combine mode
 *         shouldn't fail the whole scene load, same policy as physx_yaml.h's other parsers. */
inline dynamics::CombineMode parse_combine_mode_(const fkyaml::node& node, const std::string& key,
                                                  dynamics::CombineMode fallback) {
    if (!node.contains(key)) return fallback;
    std::string v = node.at(key).get_value<std::string>();
    if (v == "Average") return dynamics::CombineMode::Average;
    if (v == "Minimum") return dynamics::CombineMode::Minimum;
    if (v == "Maximum") return dynamics::CombineMode::Maximum;
    if (v == "Multiply") return dynamics::CombineMode::Multiply;
    return fallback;
}

} // namespace detail

/**
 * @class PhysicsMaterialLoader
 * @brief Registers as the coopa::asset loader for dynamics::PhysicsMaterial.
 *
 * @code
 * assets.register_loader<dynamics::PhysicsMaterial>(std::make_unique<loaders::PhysicsMaterialLoader>());
 * auto ice = assets.load<dynamics::PhysicsMaterial>("physics_materials/ice.yaml");
 * @endcode
 *
 * Schema (every key optional, defaults matching PhysicsMaterial's in-code defaults):
 * @code
 * dynamic_friction: 0.05
 * static_friction: 0.08
 * restitution: 0.0
 * rolling_resistance: 0.015     # wheels only -- see PhysicsMaterial::rolling_resistance
 * friction_combine: Minimum      # Average | Minimum | Maximum | Multiply
 * restitution_combine: Average
 * @endcode
 */
class PhysicsMaterialLoader : public coopa::asset::TypedAssetLoader<dynamics::PhysicsMaterial, dynamics::PhysicsMaterial> {
public:
    std::shared_ptr<dynamics::PhysicsMaterial> decode_typed(const coopa::asset::AssetId& id,
                                                              const coopa::asset::LoadContext& ctx) override {
        fkyaml::node node = coopa::yaml::load_document(ctx.resolved_path);

        auto mat = std::make_shared<dynamics::PhysicsMaterial>();
        if (node.contains("dynamic_friction")) mat->dynamic_friction = node.at("dynamic_friction").get_value<float>();
        if (node.contains("static_friction"))  mat->static_friction  = node.at("static_friction").get_value<float>();
        if (node.contains("restitution"))      mat->restitution      = node.at("restitution").get_value<float>();
        if (node.contains("rolling_resistance"))
            mat->rolling_resistance = node.at("rolling_resistance").get_value<float>();
        mat->friction_combine    = detail::parse_combine_mode_(node, "friction_combine", mat->friction_combine);
        mat->restitution_combine = detail::parse_combine_mode_(node, "restitution_combine", mat->restitution_combine);
        return mat;
    }

    std::shared_ptr<dynamics::PhysicsMaterial> finalize_typed(std::shared_ptr<dynamics::PhysicsMaterial> decoded,
                                                                const coopa::asset::AssetId&,
                                                                const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "PhysicsMaterial"; }
};

} // namespace loaders
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_LOADERS_PHYSICS_MATERIAL_LOADER_H
