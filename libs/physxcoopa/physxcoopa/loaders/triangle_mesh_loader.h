/**
 * @file triangle_mesh_loader.h
 * @brief coopa::asset loader for geometry::TriangleMesh — decodes a Blender-exported mesh YAML
 *        (the same `vertices:`/`faces:` format gfxcoopa::MeshLoader reads) into a welded,
 *        adjacency-built collision mesh.
 *
 * Unlike gfxcoopa's MeshLoader, physics has no GPU step at all: decode_typed() does the entire
 * pipeline (parse, fan-triangulate, weld, adjacency, BVH) off-thread, and finalize_typed()
 * returns its argument unchanged. See the plan's Phase 8 for why each step below is mandatory,
 * not an optimization -- in particular, welding (in TriangleMesh::from_triangles()) is required
 * for the edge-adjacency table to find shared edges at all, since exported meshes are already
 * split one vertex per face-corner.
 */

#ifndef PHYSXCOOPA_LOADERS_TRIANGLE_MESH_LOADER_H
#define PHYSXCOOPA_LOADERS_TRIANGLE_MESH_LOADER_H

#include <physxcoopa/geometry/triangle_mesh.h>

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_id.h>
#include <coopa/debug/logger.h>

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace physx {
namespace loaders {

/**
 * @class TriangleMeshLoader
 * @brief Registers as the coopa::asset loader for geometry::TriangleMesh.
 *
 * @code
 * assets.register_loader<geometry::TriangleMesh>(std::make_unique<loaders::TriangleMeshLoader>());
 * auto mesh = assets.load<geometry::TriangleMesh>("meshes/cube.000.yaml", ctx.scene_dir);
 * @endcode
 */
class TriangleMeshLoader : public coopa::asset::TypedAssetLoader<geometry::TriangleMesh, geometry::TriangleMesh> {
public:
    std::shared_ptr<geometry::TriangleMesh> decode_typed(const coopa::asset::AssetId& id,
                                                          const coopa::asset::LoadContext& ctx) override {
        fkyaml::node node = coopa::yaml::load_document(ctx.resolved_path);
        coopa::debug::Logger logger("Physics");

        // --- 1. Parse raw (unwelded) vertices ---
        std::vector<glm::vec3> raw_vertices;
        if (node.contains("vertices")) {
            for (const auto& v : node.at("vertices")) {
                raw_vertices.push_back({v.at(0).get_value<float>(), v.at(1).get_value<float>(), v.at(2).get_value<float>()});
            }
        }
        if (raw_vertices.empty()) {
            throw std::runtime_error("[physxcoopa] TriangleMeshLoader: '" + id.path() + "' has no vertices");
        }

        // --- 2. Fan-triangulate each face polygon into raw-index triangles ---
        std::vector<uint32_t> raw_indices;
        if (node.contains("faces")) {
            for (const auto& face : node.at("faces")) {
                std::vector<uint32_t> poly;
                for (const auto& idx : face) poly.push_back(idx.get_value<uint32_t>());
                if (poly.size() < 3) {
                    logger.warn("TriangleMeshLoader: skipping face with < 3 indices in '" + id.path() + "'");
                    continue;
                }
                for (size_t i = 1; i + 1 < poly.size(); ++i) {
                    raw_indices.push_back(poly[0]);
                    raw_indices.push_back(poly[i]);
                    raw_indices.push_back(poly[i + 1]);
                }
            }
        }

        // --- 3. Weld, normals, adjacency, BVH (TriangleMesh::from_triangles()) ---
        size_t non_manifold = 0;
        auto mesh = std::make_shared<geometry::TriangleMesh>(
            geometry::TriangleMesh::from_triangles(raw_vertices, raw_indices, &non_manifold));
        if (non_manifold > 0) {
            logger.warn("TriangleMeshLoader: " + std::to_string(non_manifold) + " non-manifold edge(s) in '" +
                        id.path() + "' -- left unconnected");
        }
        return mesh;
    }

    std::shared_ptr<geometry::TriangleMesh> finalize_typed(std::shared_ptr<geometry::TriangleMesh> decoded,
                                                            const coopa::asset::AssetId&,
                                                            const coopa::asset::LoadContext&) override {
        return decoded;
    }

    const char* type_name() const override { return "TriangleMesh"; }
};

} // namespace loaders
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_LOADERS_TRIANGLE_MESH_LOADER_H
