/**
 * @file skinned_mesh_source.h
 * @brief CPU-only bind-pose mesh + skinning data, parsed from a Blender-exported mesh YAML.
 *
 * toyengine has no GPU vertex skinning (no bone-matrix vertex attributes, no shader
 * skinning pass) -- an armature-driven mesh is instead skinned on the CPU each frame,
 * exactly the way toy::scene::ClothRenderer already re-derives a CPU-simulated cloth
 * sheet's vertices every frame and re-uploads them via Mesh::update_vertices()
 * (see cloth_renderer.h's file doc for why that pattern exists: this engine's pipeline
 * never waits per frame, so a dynamic mesh needs one buffer per frame-in-flight).
 * toy::scene::SkinnedMeshRenderer is the CPU-skinning counterpart to ClothRenderer,
 * holding one SkinnedMeshSource (the bind pose) and rewriting a dynamic Mesh from it
 * every frame using the current world matrix of each resolved bone SceneObject.
 *
 * Extends the plain mesh YAML schema (see mesh.h's file doc) with three more keys,
 * all optional and additive -- a mesh file with none of them still loads exactly as
 * a static coopa::gfx::engine::data::Mesh would via Mesh::from_node():
 *   joints:                 [[j0,j1,j2,j3], ...]   per-source-vertex, up to 4 palette indices
 *   joint_weights:           [[w0,w1,w2,w3], ...]   per-source-vertex, parallel to joints
 *   inverse_bind_matrices:  [[16 floats], ...]      one per palette entry, COLUMN-MAJOR
 *                                                    (glm::make_mat4() order), mapping a
 *                                                    bind-pose mesh-local vertex into that
 *                                                    palette entry's own bone-local space
 *
 * `joints`/`joint_weights` are indexed exactly like `normals`/`uvs` -- by the RAW vertex
 * index used in `vertices`, before triangulation unwelds each corner into its own Vertex
 * (see Mesh::from_node()'s identical convention for normals/uvs/tangents).
 */

#ifndef GFXCOOPA_ENGINE_DATA_SKINNED_MESH_SOURCE_H
#define GFXCOOPA_ENGINE_DATA_SKINNED_MESH_SOURCE_H

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fkYAML/node.hpp>

#include <gfxcoopa/engine/data/mesh.h>

#include <cstdint>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <string>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/**
 * @struct SkinnedMeshSource
 * @brief Bind-pose vertices/indices plus per-vertex joint palette indices/weights and
 *        per-palette-entry inverse bind matrices, decoded from a mesh YAML.
 *
 * Deliberately holds NO GPU resources (unlike Mesh) -- it is the pure-CPU intermediate
 * SkinnedMeshRenderer re-skins every frame before handing the result to
 * Mesh::from_arrays()/update_vertices(). `vertices`/`indices` are already triangulated
 * and per-corner (unwelded), exactly like Mesh::from_node()'s output, so
 * SkinnedMeshRenderer can pass them straight to Mesh::from_arrays() unchanged the first
 * time it builds its dynamic mesh.
 */
struct SkinnedMeshSource {
    std::vector<Vertex>   vertices;              /**< Bind pose, object-space, per triangle corner. */
    std::vector<uint32_t> indices;                /**< Matches vertices.size() 1:1 (no reuse), same as Mesh::from_node(). */
    std::vector<glm::ivec4> joints;                /**< Parallel to vertices; palette indices, -1 = unused slot. */
    std::vector<glm::vec4>  weights;               /**< Parallel to vertices; unnormalized as authored. */
    std::vector<glm::mat4>  inverse_bind_matrices; /**< Palette order; see this file's doc for the exact mapping. */
    /**
     * When the file has no `joints:` but does have vertex groups (`weights:`, one
     * {group name: weight} map per raw vertex -- Blender's export, and the toyeditor's Weight
     * Paint), the palette is built from them instead: `groups` lists the group names in palette
     * order and `joints`/`weights` index into it (each vertex's four strongest groups). The
     * renderer then resolves each group name as a bone of its rig. Empty otherwise.
     */
    std::vector<std::string> groups;

    /**
     * @brief Parses a SkinnedMeshSource from a mesh YAML's root fkYAML node.
     *
     * Shares Mesh::from_node()'s vertices/normals/uvs/tangents/faces parsing and fan
     * triangulation exactly (including its tangent-generation fallback when the YAML
     * carries no `tangents:`), then additionally expands `joints:`/`joint_weights:` in
     * lockstep with every other per-corner attribute, and reads `inverse_bind_matrices:`
     * verbatim (order is the caller's palette order, unvalidated here -- see
     * SkinnedMeshRenderer, which resolves each index against its own `bones:` list).
     *
     * @throws std::runtime_error if `vertices:`/`faces:` are absent or empty, matching
     *         Mesh::from_node()'s own contract.
     */
    static SkinnedMeshSource from_node(const fkyaml::node& node) {
        std::vector<glm::vec3> positions;
        if (node.contains("vertices")) {
            for (const auto& v : node.at("vertices")) {
                positions.push_back({v.at(0).get_value<float>(), v.at(1).get_value<float>(), v.at(2).get_value<float>()});
            }
        }

        std::vector<glm::vec3> normals;
        if (node.contains("normals")) {
            for (const auto& n : node.at("normals")) {
                normals.push_back({n.at(0).get_value<float>(), n.at(1).get_value<float>(), n.at(2).get_value<float>()});
            }
        }

        std::vector<glm::vec2> uvs;
        if (node.contains("uvs")) {
            for (const auto& uv : node.at("uvs")) {
                uvs.push_back({uv.at(0).get_value<float>(), uv.at(1).get_value<float>()});
            }
        }

        std::vector<glm::vec4> tangents;
        if (node.contains("tangents")) {
            for (const auto& tan : node.at("tangents")) {
                float w = (tan.size() > 3) ? tan.at(3).get_value<float>() : 1.0f;
                tangents.push_back({tan.at(0).get_value<float>(), tan.at(1).get_value<float>(), tan.at(2).get_value<float>(), w});
            }
        }

        std::vector<glm::ivec4> raw_joints;
        if (node.contains("joints")) {
            for (const auto& j : node.at("joints")) {
                glm::ivec4 v(-1);
                for (size_t i = 0; i < 4 && i < j.size(); ++i) v[static_cast<int>(i)] = j.at(i).get_value<int>();
                raw_joints.push_back(v);
            }
        }

        std::vector<glm::vec4> raw_weights;
        if (node.contains("joint_weights")) {
            for (const auto& w : node.at("joint_weights")) {
                glm::vec4 v(0.0f);
                for (size_t i = 0; i < 4 && i < w.size(); ++i) v[static_cast<int>(i)] = w.at(i).get_value<float>();
                raw_weights.push_back(v);
            }
        }

        SkinnedMeshSource out;

        // No explicit palette: derive one from the vertex groups (see `groups`).
        if (raw_joints.empty() && node.contains("weights") && node.at("weights").is_sequence()) {
            std::unordered_map<std::string, int> group_of;
            for (const auto& wmap : node.at("weights")) {
                std::vector<std::pair<float, int>> infl;
                if (wmap.is_mapping()) {
                    for (auto item : wmap.map_items()) {
                        const std::string name = item.key().get_value<std::string>();
                        const fkyaml::node& wn = item.value();
                        const float w = wn.is_integer() ? static_cast<float>(wn.get_value<int64_t>()) : wn.get_value<float>();
                        if (w <= 0.0f) continue;
                        auto it = group_of.find(name);
                        if (it == group_of.end()) {
                            it = group_of.emplace(name, static_cast<int>(out.groups.size())).first;
                            out.groups.push_back(name);
                        }
                        infl.push_back({w, it->second});
                    }
                }
                std::sort(infl.begin(), infl.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                glm::ivec4 j(-1);
                glm::vec4 w(0.0f);
                for (size_t i = 0; i < 4 && i < infl.size(); ++i) {
                    j[static_cast<int>(i)] = infl[i].second;
                    w[static_cast<int>(i)] = infl[i].first;
                }
                raw_joints.push_back(j);
                raw_weights.push_back(w);
            }
            if (out.groups.empty()) { raw_joints.clear(); raw_weights.clear(); }
        }

        if (node.contains("faces")) {
            for (const auto& face : node.at("faces")) {
                std::vector<uint32_t> face_indices;
                for (const auto& idx_node : face) face_indices.push_back(idx_node.get_value<uint32_t>());
                if (face_indices.size() < 3) continue;

                for (size_t i = 1; i + 1 < face_indices.size(); ++i) {
                    uint32_t v_indices[3] = {face_indices[0], face_indices[i], face_indices[i + 1]};
                    for (uint32_t vi : v_indices) {
                        Vertex vert{};
                        vert.position = (vi < positions.size()) ? positions[vi] : glm::vec3(0.0f);
                        vert.normal   = (vi < normals.size())   ? normals[vi]   : glm::vec3(0.0f, 1.0f, 0.0f);
                        vert.uv       = (vi < uvs.size())       ? uvs[vi]       : glm::vec2(0.0f);
                        vert.tangent  = (vi < tangents.size())  ? tangents[vi]  : glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                        out.vertices.push_back(vert);
                        out.indices.push_back(static_cast<uint32_t>(out.indices.size()));

                        out.joints.push_back((vi < raw_joints.size()) ? raw_joints[vi] : glm::ivec4(-1));
                        out.weights.push_back((vi < raw_weights.size()) ? raw_weights[vi] : glm::vec4(0.0f));
                    }
                }
            }
        }

        if (out.vertices.empty()) {
            throw std::runtime_error("[SkinnedMeshSource] No vertices parsed -- empty or invalid mesh YAML.");
        }

        // Fallback tangent computation, identical to Mesh::from_node()'s -- see that
        // method's own comments for the derivation.
        if (tangents.empty()) {
            std::vector<glm::vec3> tan_sum(out.vertices.size(), glm::vec3(0.0f));
            for (size_t i = 0; i + 2 < out.vertices.size(); i += 3) {
                const auto& v0 = out.vertices[i];
                const auto& v1 = out.vertices[i + 1];
                const auto& v2 = out.vertices[i + 2];
                glm::vec3 edge1 = v1.position - v0.position;
                glm::vec3 edge2 = v2.position - v0.position;
                glm::vec2 deltaUV1 = v1.uv - v0.uv;
                glm::vec2 deltaUV2 = v2.uv - v0.uv;
                float f = (deltaUV1.x * deltaUV2.y - deltaUV2.x * deltaUV1.y);
                glm::vec3 t = (std::abs(f) > 1e-6f)
                    ? (edge1 * deltaUV2.y - edge2 * deltaUV1.y) * (1.0f / f)
                    : glm::vec3(1.0f, 0.0f, 0.0f);
                tan_sum[i] += t; tan_sum[i + 1] += t; tan_sum[i + 2] += t;
            }
            for (size_t i = 0; i < out.vertices.size(); ++i) {
                glm::vec3 t = glm::length(tan_sum[i]) > 1e-5f ? glm::normalize(tan_sum[i]) : glm::vec3(1.0f, 0.0f, 0.0f);
                glm::vec3 n = out.vertices[i].normal;
                t = glm::normalize(t - n * glm::dot(n, t));
                out.vertices[i].tangent = glm::vec4(t, 1.0f);
            }
        }

        if (node.contains("inverse_bind_matrices")) {
            for (const auto& m : node.at("inverse_bind_matrices")) {
                float flat[16];
                for (size_t i = 0; i < 16 && i < m.size(); ++i) flat[i] = m.at(i).get_value<float>();
                out.inverse_bind_matrices.push_back(glm::make_mat4(flat));
            }
        }

        return out;
    }
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_DATA_SKINNED_MESH_SOURCE_H
