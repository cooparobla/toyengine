/**
 * @file skinned_mesh_source_loader.h
 * @brief coopa::asset loader for gfx::data::SkinnedMeshSource -- a pure-CPU asset.
 *
 * Unlike MeshLoader, there is no GPU work at all: SkinnedMeshSource holds only bind-pose
 * vertices/indices/joints/weights/inverse-bind-matrices (see that file's doc), and
 * toy::scene::SkinnedMeshRenderer re-skins them into an ordinary dynamic
 * coopa::gfx::engine::data::Mesh every frame. decode_typed() therefore does the whole
 * job off-thread; finalize_typed() is a pass-through, the same shape
 * coopa::anim::AnimationClipLoader uses for the same reason.
 */

#ifndef GFXCOOPA_ENGINE_LOADERS_SKINNED_MESH_SOURCE_LOADER_H
#define GFXCOOPA_ENGINE_LOADERS_SKINNED_MESH_SOURCE_LOADER_H

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_id.h>

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>

#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace loaders {

/**
 * @class SkinnedMeshSourceLoader
 * @brief Registers as the coopa::asset loader for gfx::data::SkinnedMeshSource.
 *
 * @code
 * assets.register_loader<gfx::data::SkinnedMeshSource>(
 *     std::make_unique<SkinnedMeshSourceLoader>());
 * auto src = assets.load_async<gfx::data::SkinnedMeshSource>("meshes/character.yaml", ctx.scene_dir);
 * @endcode
 */
class SkinnedMeshSourceLoader : public coopa::asset::TypedAssetLoader<data::SkinnedMeshSource> {
public:
    std::shared_ptr<data::SkinnedMeshSource> decode_typed(const coopa::asset::AssetId& id,
                                                          const coopa::asset::LoadContext& ctx) override {
        fkyaml::node node = coopa::yaml::load_document(ctx.resolved_path);
        return std::make_shared<data::SkinnedMeshSource>(data::SkinnedMeshSource::from_node(node));
    }

    std::shared_ptr<data::SkinnedMeshSource> finalize_typed(std::shared_ptr<data::SkinnedMeshSource> decoded,
                                                             const coopa::asset::AssetId&,
                                                             const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "SkinnedMeshSource"; }
};

} // namespace loaders
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_LOADERS_SKINNED_MESH_SOURCE_LOADER_H
