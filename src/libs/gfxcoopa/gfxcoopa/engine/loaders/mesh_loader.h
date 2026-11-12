/**
 * @file mesh_loader.h
 * @brief coopa::asset loader for gfx::data::Mesh — reads and parses a Blender-exported mesh YAML.
 *
 * decode_typed() runs off-thread and only reads + parses the YAML file (pure
 * CPU work via fkYAML); finalize_typed() runs on the main thread and is
 * where the GPU vertex/index buffers actually get created and uploaded
 * (via data::Mesh::from_node).
 *
 * Caching is AssetManager's, keyed on the *resolved* filesystem path (see
 * coopa/asset/asset_id.h) rather than the logical mesh name. Two scenes that
 * each contain a different meshes/cube.000.yaml therefore get their own
 * upload instead of silently sharing one.
 */

#ifndef GFXCOOPA_ENGINE_LOADERS_MESH_LOADER_H
#define GFXCOOPA_ENGINE_LOADERS_MESH_LOADER_H

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_id.h>

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/mesh.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace loaders {

/**
 * @class MeshLoader
 * @brief Registers as the coopa::asset loader for gfx::data::Mesh.
 *
 * @code
 * assets.register_loader<gfx::data::Mesh>(
 *     std::make_unique<MeshLoader>(device, allocator, cmd_pool));
 * auto mesh = assets.load<gfx::data::Mesh>("meshes/cube.000.yaml", ctx.scene_dir);
 * @endcode
 */
class MeshLoader : public coopa::asset::TypedAssetLoader<data::Mesh, data::MeshCpuData> {
public:
    MeshLoader(core::Device& device, memory::Allocator& allocator, command::CommandPool& cmd_pool)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool) {}

    /// Worker thread: parse, weld, optimize and build LODs (Mesh::build_cpu). A
    /// `<mesh>.lod.yaml` sidecar next to the mesh file, when present, supplies the LOD setup
    /// in place of the mesh's own `lods` block; a level's `mesh: name` resolves to
    /// `name.yaml` in the same directory.
    std::shared_ptr<data::MeshCpuData> decode_typed(const coopa::asset::AssetId& id,
                                                    const coopa::asset::LoadContext& ctx) override {
        auto read = [](const std::filesystem::path& path) -> std::shared_ptr<fkyaml::node> {
            std::optional<fkyaml::node> node = coopa::yaml::try_load_document(coopa::yaml::resolve_variant(path));
            if (!node) return nullptr;
            return std::make_shared<fkyaml::node>(std::move(*node));
        };
        const std::filesystem::path path(ctx.resolved_path);
        std::shared_ptr<fkyaml::node> node = read(path);
        if (!node) {
            throw std::runtime_error("[MeshLoader] Failed to open '" + id.path() + "'");
        }

        // "x.yaml" and "x.caml" both look for "x.lod.yaml", which read() also finds as "x.lod.caml".
        std::filesystem::path sidecar = path;
        sidecar.replace_extension(".lod.yaml");
        std::shared_ptr<fkyaml::node> lod_cfg = read(sidecar);

        const std::filesystem::path dir = path.parent_path();
        auto load_sibling = [&read, &dir](const std::string& name) { return read(dir / (name + ".yaml")); };

        return std::make_shared<data::MeshCpuData>(
            data::Mesh::build_cpu(*node, lod_cfg.get(), load_sibling));
    }

    /// Main thread: upload only.
    std::shared_ptr<data::Mesh> finalize_typed(std::shared_ptr<data::MeshCpuData> cpu,
                                               const coopa::asset::AssetId&,
                                               const coopa::asset::LoadContext&) override {
        return std::make_shared<data::Mesh>(data::Mesh::from_cpu(device_, allocator_, std::move(*cpu)));
    }

    const char* type_name() const override { return "Mesh"; }

private:
    core::Device&         device_;
    memory::Allocator&     allocator_;
    command::CommandPool&  cmd_pool_;
};

} // namespace loaders
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_LOADERS_MESH_LOADER_H
