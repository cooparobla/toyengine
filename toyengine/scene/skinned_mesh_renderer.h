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
 * A skinned mesh's bones are ordinary SceneObjects -- a rig is just an object hierarchy, its
 * bones animated by Transform tracks of an Animator on the rig root (coopa::anim; there is no
 * separate skeletal system). Bones are resolved against the RIG: the `rig:` path if given,
 * else the nearest ancestor (or the owner itself) carrying an Animator, else the owner's
 * topmost ancestor. A bone entry is a path below the rig root ("Hips/Spine") or a bare name
 * found anywhere under it, with a scene-wide path lookup as the fallback.
 *
 * The palette comes from either source:
 *  - `bones:` in the mesh's `joints:` / `inverse_bind_matrices:` order (an exporter's), or
 *  - with no `bones:` and a mesh whose vertex groups name the bones (`weights:` -- Blender's
 *    export, and the toyeditor's Weight Paint), the groups themselves: group "Forearm" binds
 *    to the rig object named "Forearm".
 * Inverse bind matrices come from the file when it has them; otherwise from the REST pose --
 * every rig object's world matrix as authored, snapshotted in start() before any animation
 * has moved it -- so a mesh painted in the editor needs no export step at all.
 */

#ifndef TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
#define TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/animation/animator.h>
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
 *   bones: [Hips, Hips/Spine, Hips/Spine/Chest]   # or omit: the mesh's vertex groups name them
 *   rig: Character                                # optional; default: the nearest Animator
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

    /** @brief Sets the bone palette: paths under the rig, in mesh joint-index order (empty: the mesh's vertex groups). */
    void set_bones(std::vector<std::string> bones) { bone_paths_ = std::move(bones); }
    /** @brief Names the rig root explicitly (a scene path); empty = the nearest Animator ancestor. */
    void set_rig(std::string rig) { rig_path_ = std::move(rig); }

    /**
     * @brief Finds the sibling MeshRenderer and the rig root, and snapshots the REST pose:
     *        the world matrix of the owner and of every object under the rig, before any
     *        animation runs (the bind pose for meshes whose file has no inverse binds).
     *        Bones themselves are resolved once the mesh source has loaded (ensure_mesh_()),
     *        since with no `bones:` they are named by the mesh's vertex groups.
     */
    void start() override {
        if (!owner) return;
        renderer_ = owner->get_component<MeshRenderer>();
        rig_ = find_rig_();
        rest_world_.clear();
        if (auto* tc = owner->get_transform()) owner_rest_world_ = tc->get_world_matrix();
        if (rig_) snapshot_rest_(rig_);
    }

    /** @brief The rig root bones are resolved under (after start()). */
    coopa::scene::SceneObject* rig() const { return rig_; }
    /** @brief The resolved palette (after the mesh loaded); entries may be null. */
    const std::vector<coopa::scene::SceneObject*>& bones() const { return bones_; }

    /**
     * @brief Skins `src`'s bind-pose vertices with one matrix per palette entry into `out`
     *        (object space of the mesh's owner). A vertex with no usable influence keeps its
     *        bind pose. Pure; the per-frame upload's whole CPU cost.
     */
    static void skin(const SkinnedMeshSource& src, const std::vector<glm::mat4>& skin_matrices, std::vector<Vertex>& out) {
        out.resize(src.vertices.size());
        for (size_t i = 0; i < src.vertices.size(); ++i) {
            const glm::ivec4& j = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
            const glm::vec4&  w = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);
            glm::vec3 pos(0.0f), nrm(0.0f), tan(0.0f);
            float weight_sum = 0.0f;
            for (int c = 0; c < 4; ++c) {
                if (j[c] < 0 || w[c] <= 0.0f || static_cast<size_t>(j[c]) >= skin_matrices.size()) continue;
                const glm::mat4& m = skin_matrices[static_cast<size_t>(j[c])];
                pos += w[c] * glm::vec3(m * glm::vec4(src.vertices[i].position, 1.0f));
                // Rigid/uniformly-scaled bones only (the common case for a character rig) --
                // no inverse-transpose here, matching the same simplification most small
                // engines make for skinning normals.
                nrm += w[c] * glm::vec3(m * glm::vec4(src.vertices[i].normal, 0.0f));
                tan += w[c] * glm::vec3(m * glm::vec4(glm::vec3(src.vertices[i].tangent), 0.0f));
                weight_sum += w[c];
            }
            if (weight_sum > 1e-6f) {
                out[i] = src.vertices[i];
                out[i].position = pos / weight_sum;
                const float n_len = glm::length(nrm);
                out[i].normal = (n_len > 1e-6f) ? nrm / n_len : src.vertices[i].normal;
                const float t_len = glm::length(tan);
                const glm::vec3 t = (t_len > 1e-6f) ? tan / t_len : glm::vec3(src.vertices[i].tangent);
                out[i].tangent = glm::vec4(t, src.vertices[i].tangent.w);
            } else {
                // No (or zero-weight) influences on this vertex -- leave it at the bind pose
                // rather than collapsing it to the origin.
                out[i] = src.vertices[i];
            }
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

    /**
     * @brief Drops the GPU mesh so the next upload rebuilds it from the (new) source -- for a tool
     *        replacing the bind pose (the toyeditor editing the mesh). Bones are re-resolved against
     *        the rest pose captured in start(), never the current animated one.
     */
    void rebuild() {
        mesh_.reset();
        vertices_.clear();
    }

    /** @brief True once the GPU mesh exists and is bound to the sibling MeshRenderer. */
    bool is_ready() const { return mesh_ != nullptr; }
    /** @brief The last skinned vertices uploaded (object space; tests and tools). */
    const std::vector<Vertex>& skinned_vertices() const { return vertices_; }

private:
    /**
     * @brief Allocates the dynamic GPU mesh from the loaded source's bind pose and binds it
     *        to the sibling MeshRenderer. Idempotent; returns false until `source_` is loaded.
     */
    bool ensure_mesh_() {
        if (!source_.is_loaded() || !renderer_) return false;
        const SkinnedMeshSource& src = *source_;
        if (src.vertices.empty() || src.indices.empty()) return false;
        resolve_bones_(src);

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
        auto* tc = owner ? owner->get_transform() : nullptr;
        const glm::mat4 inv_owner_world = tc ? glm::inverse(tc->get_world_matrix()) : glm::mat4(1.0f);
        skin_matrices_.assign(bones_.size(), glm::mat4(1.0f));
        for (size_t i = 0; i < bones_.size(); ++i) {
            if (!bones_[i]) continue; // unresolved bone -- stays identity, see resolve_bones_()
            auto* bone_tc = bones_[i]->get_transform();
            if (!bone_tc) continue;
            skin_matrices_[i] = inv_owner_world * bone_tc->get_world_matrix() * inverse_bind_[i];
        }
        skin(*source_, skin_matrices_, vertices_);
    }

    /** @brief The rig root: `rig:`, else the nearest Animator up the hierarchy, else the topmost ancestor. */
    coopa::scene::SceneObject* find_rig_() const {
        if (!rig_path_.empty() && scene) {
            if (auto* r = scene->find_object_by_path(rig_path_)) return r;
        }
        for (coopa::scene::SceneObject* o = owner; o; o = o->parent()) {
            if (o->get_component<coopa::anim::Animator>()) return o;
        }
        coopa::scene::SceneObject* top = owner;
        while (top && top->parent()) top = top->parent();
        return top;
    }

    void snapshot_rest_(coopa::scene::SceneObject* o) {
        if (auto* tc = o->get_transform()) rest_world_[o] = tc->get_world_matrix();
        for (const auto& c : o->children()) snapshot_rest_(c.get());
    }

    /** @brief A bone path under the rig: "A/B" walks down from the rig root; a bare name may sit at any depth. */
    coopa::scene::SceneObject* find_bone_(const std::string& path) const {
        if (rig_) {
            if (path == rig_->name()) return rig_;
            coopa::scene::SceneObject* cur = rig_;
            size_t start = 0;
            bool first = true;
            while (cur && start <= path.size()) {
                const size_t slash = path.find('/', start);
                const std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                cur = first ? cur->find_descendant(seg) : cur->find_child(seg);
                first = false;
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            if (cur) return cur;
        }
        return scene ? scene->find_object_by_path(path) : nullptr;   // the scene-wide form
    }

    /** @brief Resolves the palette and its inverse bind matrices (file, else rest pose). */
    void resolve_bones_(const SkinnedMeshSource& src) {
        const std::vector<std::string>& names = !bone_paths_.empty() ? bone_paths_ : src.groups;
        bones_.clear();
        inverse_bind_.clear();
        for (size_t i = 0; i < names.size(); ++i) {
            coopa::scene::SceneObject* b = find_bone_(names[i]);
            bones_.push_back(b);
            glm::mat4 inv_bind(1.0f);
            if (i < src.inverse_bind_matrices.size()) {
                inv_bind = src.inverse_bind_matrices[i];
            } else if (b) {
                // Rest pose: identity skinning at the authored pose. bone_world_rest^-1 * owner_world_rest.
                auto it = rest_world_.find(b);
                const glm::mat4 bone_rest = it != rest_world_.end() ? it->second
                                          : (b->get_transform() ? b->get_transform()->get_world_matrix() : glm::mat4(1.0f));
                inv_bind = glm::inverse(bone_rest) * owner_rest_world_;
            }
            inverse_bind_.push_back(inv_bind);
        }
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;
    uint32_t                       frames_in_flight_;

    coopa::asset::AssetHandle<SkinnedMeshSource> source_;
    std::vector<std::string> bone_paths_;
    std::string rig_path_;
    coopa::scene::SceneObject* rig_ = nullptr;
    std::vector<coopa::scene::SceneObject*> bones_; // resolved when the source loads; may contain nullptr
    std::vector<glm::mat4> inverse_bind_;           // parallel to bones_
    std::unordered_map<const coopa::scene::SceneObject*, glm::mat4> rest_world_;   // start()'s snapshot
    glm::mat4 owner_rest_world_{1.0f};

    MeshRenderer*             renderer_ = nullptr;
    std::shared_ptr<Mesh>     mesh_;
    std::vector<Vertex>       vertices_;
    std::vector<glm::mat4>    skin_matrices_;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
