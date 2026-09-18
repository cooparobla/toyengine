/**
 * @file skinned_mesh_renderer.h
 * @brief CPU-skins a bind-pose SkinnedMeshSource against animated bone SceneObjects every
 *        frame, uploading the result into a sibling MeshRenderer's dynamic GPU mesh.
 *
 * toyengine has no GPU vertex skinning -- no bone-matrix vertex attributes, no shader
 * skinning pass (see gfxcoopa/engine/data/skinned_mesh_source.h's file doc). Instead this
 * mirrors ClothRenderer's already-proven pattern exactly: a sibling MeshRenderer supplies
 * the material and the draw, this component supplies the mesh, and Engine::upload_dynamic_meshes_()
 * calls upload() once per frame, after Scene::late_update() (so animated bone Transforms are
 * current) and before the frame's command buffer is recorded.
 *
 * A skinned mesh's bones are ordinary animated SceneObjects (see blendy's export: an
 * armature's bones become scene-graph children with baked Transform keyframe tracks --
 * there is no separate skeletal animation system in coopa::anim, just Transform tracks
 * addressed by bone name). `bones:` names them in the same palette order the mesh's
 * `joints:`/`inverse_bind_matrices:` use.
 */

#ifndef TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
#define TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <gfxcoopa/memory/allocator.h>

namespace toy {
namespace scene {

/**
 * @class SkinnedMeshRenderer
 * @brief Builds and maintains a CPU-skinned dynamic GPU mesh for a sibling MeshRenderer.
 *
 * Requires a sibling `MeshRenderer` (material + draw), authored with no `mesh_path` of its
 * own -- exactly ClothRenderer's convention, for the same reason: this component supplies
 * the mesh instead.
 *
 * Example YAML:
 * @code
 * - type: MeshRenderer
 *   material: { albedo: { r: 0.8, g: 0.8, b: 0.8 }, roughness: 0.6 }
 * - type: SkinnedMeshRenderer
 *   mesh_path: character_body
 *   bones: [Hips, Hips/Spine, Hips/Spine/Chest]
 * @endcode
 */
class SkinnedMeshRenderer : public coopa::scene::Component {
public:
    using Mesh              = coopa::gfx::engine::data::Mesh;
    using Vertex            = coopa::gfx::engine::data::Vertex;
    using SkinnedMeshSource = coopa::gfx::engine::data::SkinnedMeshSource;
    using MeshRenderer      = coopa::gfx::engine::components::MeshRenderer;

    SkinnedMeshRenderer(coopa::gfx::core::Device& device,
                        coopa::gfx::memory::Allocator& allocator,
                        coopa::asset::AssetManager& assets,
                        uint32_t frames_in_flight)
        : device_(device), allocator_(allocator), assets_(assets),
          frames_in_flight_(frames_in_flight < 1u ? 1u : frames_in_flight) {}

    std::string type_name() const override { return "SkinnedMeshRenderer"; }

    /** @brief Sets the loaded (still possibly in-flight) bind-pose source. Called by the parser. */
    void set_source(coopa::asset::AssetHandle<SkinnedMeshSource> source) { source_ = std::move(source); }

    /** @brief Sets the bone palette: SceneObject paths, resolved in start(), in mesh joint-index order. */
    void set_bones(std::vector<std::string> bones) { bone_paths_ = std::move(bones); }

    /**
     * @brief Resolves the sibling MeshRenderer and every named bone against the scene.
     *
     * Bone resolution happens here rather than lazily in upload(): Scene::start() has, by
     * this point, fully constructed every object in the scene (see Scene::start()'s doc --
     * component `scene` back-pointers are stamped before any component's start() runs), so
     * every bone this mesh references is guaranteed to already exist as a SceneObject.
     * A bone that still fails to resolve (typo, or a mesh authored against the wrong
     * armature) leaves a null entry in bones_ -- skin_matrices_() then falls back to
     * identity for that palette slot rather than crashing.
     */
    void start() override {
        if (!owner) return;
        renderer_ = owner->get_component<MeshRenderer>();
        bones_.reserve(bone_paths_.size());
        for (const std::string& path : bone_paths_) {
            bones_.push_back(scene ? scene->find_object_by_path(path) : nullptr);
        }
    }

    /**
     * @brief Builds the mesh if needed, re-skins this frame's vertices, and uploads them.
     *
     * No-op until `source_` finishes loading (async), matching ClothRenderer's own
     * "starts drawing one frame later" contract for a dependency not ready at start().
     *
     * @param frame_slot The renderer's current in-flight frame index.
     */
    void upload(uint32_t frame_slot) {
        if (!mesh_ && !ensure_mesh_()) return;
        rebuild_vertices_();
        mesh_->update_vertices(vertices_.data(), vertices_.size(), frame_slot);
    }

    /** @brief True once the GPU mesh exists and is bound to the sibling MeshRenderer. */
    bool is_ready() const { return mesh_ != nullptr; }

private:
    /**
     * @brief Allocates the dynamic GPU mesh from the loaded source's bind pose and binds it
     *        to the sibling MeshRenderer. Idempotent; returns false until `source_` is loaded.
     */
    bool ensure_mesh_() {
        if (!source_.is_loaded() || !renderer_) return false;
        const SkinnedMeshSource& src = *source_;
        if (src.vertices.empty() || src.indices.empty()) return false;

        vertices_ = src.vertices; // bind pose; rebuild_vertices_() below re-skins in place
        mesh_ = std::make_shared<Mesh>(
            Mesh::from_arrays(device_, allocator_, vertices_, src.indices, frames_in_flight_));

        // Synthetic id, prefixed out of the real-path namespace -- see
        // AssetManager::create()'s doc and ClothRenderer::ensure_mesh_()'s identical use.
        renderer_->set_mesh(assets_.create<Mesh>("runtime/skinned/" + owner->name(), mesh_));
        return true;
    }

    /**
     * @brief Re-skins every vertex from the bind pose using this frame's bone world matrices.
     *
     * Per bone i: skin_matrix[i] = inverse(this object's world matrix) * bone_world[i] *
     * inverse_bind_matrices[i]. The middle two terms alone would leave the result in WORLD
     * space; left-multiplying by the inverse of this renderer's own owner's world matrix
     * brings it back into that owner's object space, which is what a MeshRenderer's
     * vertices are expected to be in (they get multiplied by that same world matrix again
     * at draw time via the per-instance model matrix) -- the identical "don't double-apply
     * the owner's transform" concern ClothRenderer::rebuild_vertices() documents for its
     * world-space particles.
     *
     * At bind time (every bone at its rest pose) this reduces to the identity for every
     * bone: inverse_bind_matrices[i] is exported as the inverse of exactly
     * `inverse(mesh_world_at_bind) * bone_world_at_bind[i]`, so a weighted blend of
     * identities reproduces the authored bind-pose vertex unchanged, which is the standard
     * skinning correctness check.
     */
    void rebuild_vertices_() {
        const SkinnedMeshSource& src = *source_;
        auto* tc = owner ? owner->get_transform() : nullptr;
        const glm::mat4 inv_owner_world = tc ? glm::inverse(tc->get_world_matrix()) : glm::mat4(1.0f);

        skin_matrices_.assign(bones_.size(), glm::mat4(1.0f));
        for (size_t i = 0; i < bones_.size(); ++i) {
            if (!bones_[i]) continue; // unresolved bone -- stays identity, see start()'s doc
            auto* bone_tc = bones_[i]->get_transform();
            if (!bone_tc) continue;
            const glm::mat4 inv_bind = (i < src.inverse_bind_matrices.size())
                ? src.inverse_bind_matrices[i] : glm::mat4(1.0f);
            skin_matrices_[i] = inv_owner_world * bone_tc->get_world_matrix() * inv_bind;
        }

        for (size_t i = 0; i < vertices_.size(); ++i) {
            const glm::ivec4& j = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
            const glm::vec4&  w = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);

            glm::vec3 pos(0.0f), nrm(0.0f), tan(0.0f);
            float weight_sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                if (j[c] < 0 || w[c] <= 0.0f || static_cast<size_t>(j[c]) >= skin_matrices_.size()) continue;
                const glm::mat4& m = skin_matrices_[static_cast<size_t>(j[c])];
                pos += w[c] * glm::vec3(m * glm::vec4(src.vertices[i].position, 1.0f));
                // Rigid/uniformly-scaled bones only (the common case for a character rig) --
                // no inverse-transpose here, matching the same simplification most small
                // engines make for skinning normals.
                nrm += w[c] * glm::vec3(m * glm::vec4(src.vertices[i].normal, 0.0f));
                tan += w[c] * glm::vec3(m * glm::vec4(glm::vec3(src.vertices[i].tangent), 0.0f));
                weight_sum += w[c];
            }

            if (weight_sum > 1e-6f) {
                vertices_[i].position = pos / weight_sum;
                float n_len = glm::length(nrm);
                vertices_[i].normal = (n_len > 1e-6f) ? nrm / n_len : src.vertices[i].normal;
                float t_len = glm::length(tan);
                glm::vec3 t = (t_len > 1e-6f) ? tan / t_len : glm::vec3(src.vertices[i].tangent);
                vertices_[i].tangent = glm::vec4(t, src.vertices[i].tangent.w);
            } else {
                // No (or zero-weight) influences on this vertex -- leave it at the bind pose
                // rather than collapsing it to the origin.
                vertices_[i] = src.vertices[i];
            }
        }
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;
    uint32_t                       frames_in_flight_;

    coopa::asset::AssetHandle<SkinnedMeshSource> source_;
    std::vector<std::string> bone_paths_;
    std::vector<coopa::scene::SceneObject*> bones_; // resolved in start(); may contain nullptr

    MeshRenderer*             renderer_ = nullptr;
    std::shared_ptr<Mesh>     mesh_;
    std::vector<Vertex>       vertices_;
    std::vector<glm::mat4>    skin_matrices_;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
