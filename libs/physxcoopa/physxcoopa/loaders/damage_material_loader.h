/**
 * @file damage_material_loader.h
 * @brief coopa::asset loader for damage::DamageMaterial -- decodes a small YAML document into a
 *        shareable, AssetManager-owned material. Same convention as PhysicsMaterialLoader.
 */

#ifndef PHYSXCOOPA_LOADERS_DAMAGE_MATERIAL_LOADER_H
#define PHYSXCOOPA_LOADERS_DAMAGE_MATERIAL_LOADER_H

#include <physxcoopa/damage/damage_material.h>
#include <physxcoopa/damage/damage_types.h>

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_id.h>

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>

namespace coopa {
namespace physx {
namespace loaders {

/**
 * @class DamageMaterialLoader
 * @brief Registers as the coopa::asset loader for damage::DamageMaterial.
 *
 * @code
 * assets.register_loader<damage::DamageMaterial>(std::make_unique<loaders::DamageMaterialLoader>());
 * auto wood = assets.load<damage::DamageMaterial>("damage_materials/wood.yaml");
 * @endcode
 *
 * Schema (every key optional, defaults matching DamageMaterial's in-code defaults):
 * @code
 * hardness: 0.3
 * toughness: 0.6
 * fibrous: 0.8
 * hit_points: 100
 * resistance: { impact: 1.0, slash: 0.6, pierce: 1.2, explosive: 1.0, fire: 1.0 }
 * @endcode
 */
class DamageMaterialLoader : public coopa::asset::TypedAssetLoader<damage::DamageMaterial, damage::DamageMaterial> {
public:
    std::shared_ptr<damage::DamageMaterial> decode_typed(const coopa::asset::AssetId&,
                                                           const coopa::asset::LoadContext& ctx) override {
        fkyaml::node node = coopa::yaml::load_document(ctx.resolved_path);

        auto mat = std::make_shared<damage::DamageMaterial>();
        if (node.contains("hardness"))   mat->hardness   = node.at("hardness").get_value<float>();
        if (node.contains("toughness"))  mat->toughness  = node.at("toughness").get_value<float>();
        if (node.contains("fibrous"))    mat->fibrous    = node.at("fibrous").get_value<float>();
        if (node.contains("hit_points")) mat->hit_points = node.at("hit_points").get_value<float>();
        if (node.contains("resistance") && node.at("resistance").is_mapping()) {
            const fkyaml::node& r = node.at("resistance");
            auto read = [&](const char* key, damage::DamageType type) {
                if (r.contains(key)) mat->resistance[static_cast<std::size_t>(type)] = r.at(key).get_value<float>();
            };
            read("impact", damage::DamageType::Impact);
            read("slash", damage::DamageType::Slash);
            read("pierce", damage::DamageType::Pierce);
            read("explosive", damage::DamageType::Explosive);
            read("fire", damage::DamageType::Fire);
        }
        return mat;
    }

    std::shared_ptr<damage::DamageMaterial> finalize_typed(std::shared_ptr<damage::DamageMaterial> decoded,
                                                             const coopa::asset::AssetId&,
                                                             const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "DamageMaterial"; }
};

} // namespace loaders
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_LOADERS_DAMAGE_MATERIAL_LOADER_H
