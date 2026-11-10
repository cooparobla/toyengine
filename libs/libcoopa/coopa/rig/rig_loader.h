/**
 * @file rig_loader.h
 * @brief Parses RigAsset from YAML and adapts that into an asset loader.
 */

#ifndef COOPA_RIG_RIG_LOADER_H
#define COOPA_RIG_RIG_LOADER_H

#include <coopa/asset/asset_loader.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/yaml/document.h>
#include <fkYAML/node.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace coopa {
namespace rig {

/** @brief Reads an {x,y,z} mapping into a glm::vec3, defaulting missing components to 0. */
inline glm::vec3 parse_rig_vec3(const fkyaml::node& node) {
    glm::vec3 out(0.0f);
    if (!node.is_mapping()) return out;
    if (node.contains("x")) out.x = node.at("x").get_value<float>();
    if (node.contains("y")) out.y = node.at("y").get_value<float>();
    if (node.contains("z")) out.z = node.at("z").get_value<float>();
    return out;
}

/**
 * @brief Parses one Bone from its YAML node. `parent:` names an already-parsed bone (the
 *        list is parent-first) — resolved against `bones_so_far` so a bone never needs a
 *        forward reference.
 */
inline Bone parse_bone(const fkyaml::node& node, const std::vector<Bone>& bones_so_far) {
    Bone bone;
    if (node.contains("name")) bone.name = node.at("name").get_value<std::string>();
    if (node.contains("head")) bone.head = parse_rig_vec3(node.at("head"));
    if (node.contains("tail")) bone.tail = parse_rig_vec3(node.at("tail"));
    if (node.contains("roll")) bone.roll = node.at("roll").get_value<float>();
    if (node.contains("connected")) bone.connected = node.at("connected").get_value<bool>();
    if (node.contains("parent")) {
        const std::string parent_name = node.at("parent").get_value<std::string>();
        for (size_t i = 0; i < bones_so_far.size(); ++i) {
            if (bones_so_far[i].name == parent_name) {
                bone.parent = static_cast<int>(i);
                break;
            }
        }
    }
    return bone;
}

/**
 * @brief Parses a full RigAsset from an already-deserialized YAML root node.
 *
 * Expects the `rig:` mapping form: a `name:` and a parent-first `bones:` sequence, each
 * entry a `name:` plus rest-pose `head:`/`tail:`/`roll:`, an optional `parent:` (an
 * already-listed bone's name) and `connected:`.
 *
 * @throws std::runtime_error if the root has no `rig:` key.
 */
inline RigAsset parse_rig(const fkyaml::node& root) {
    if (!root.contains("rig")) {
        throw std::runtime_error("[RigAsset] root node has no 'rig:' key");
    }
    const auto& node = root.at("rig");

    RigAsset rig;
    if (node.contains("name")) rig.name = node.at("name").get_value<std::string>();
    if (node.contains("bones")) {
        for (const auto& bone_node : node.at("bones")) {
            rig.bones.push_back(parse_bone(bone_node, rig.bones));
        }
    }
    rig.compute_rest();
    return rig;
}

/**
 * @class RigAssetLoader
 * @brief coopa::asset TypedAssetLoader for RigAsset — a pure-CPU asset.
 *
 * decode_typed() runs entirely on the asset IO worker thread: a rig stores only strings,
 * floats and vectors, with every bone matrix derived rather than looked up against a scene.
 * finalize_typed() is a pass-through.
 */
class RigAssetLoader : public coopa::asset::TypedAssetLoader<RigAsset> {
public:
    std::shared_ptr<RigAsset> decode_typed(const coopa::asset::AssetId&,
                                           const coopa::asset::LoadContext& ctx) override {
        fkyaml::node root = coopa::yaml::load_document(ctx.resolved_path);
        return std::make_shared<RigAsset>(parse_rig(root));
    }

    std::shared_ptr<RigAsset> finalize_typed(std::shared_ptr<RigAsset> decoded,
                                             const coopa::asset::AssetId&,
                                             const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "RigAsset"; }
};

} // namespace rig
} // namespace coopa

#endif // COOPA_RIG_RIG_LOADER_H
