/**
 * @file skinned_mesh_renderer.h
 * @brief Skins a bind-pose SkinnedMeshSource against animated bone SceneObjects every frame
 *        into a sibling MeshRenderer's dynamic GPU mesh -- on the GPU (a compute pre-skin pass,
 *        render/passes/skinning_pass.h) or, as the fallback, on the CPU.
 *
 * `render.skinning: gpu` (the default, when the device has compute): upload() builds this
 * frame's bone palette and enqueues one dispatch that writes the skinned vertices into the
 * mesh's per-slot vertex buffer; nothing is uploaded per vertex. `skinning: cpu` (or no
 * compute): skin() runs here and update_vertices() uploads the result. Either way the draws
 * bind the same buffer, so no consumer knows which path wrote it.
 *
 * There are no bone-matrix vertex attributes: skinning is a pre-pass, and the result
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

#include <algorithm>
#include <limits>
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
#include <gfxcoopa/memory/storage_buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

#include "toyengine/render/passes/skinning_pass.h"

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

    /** @brief The compute path's buffers may still be read by an in-flight frame: drain first
     *         (editor edits and destroying a skinned object -- rare, so a stall is acceptable). */
    ~SkinnedMeshRenderer() override {
        if (gpu_) device_.wait_idle();
    }

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
     * @brief Per bone: inverse(owner_world) * bone_world * inverse_bind -- the skin matrix that
     *        takes a bind-pose vertex (owner object space) to its posed position, again in the
     *        owner's object space. A null bone stays identity. Pure (tests).
     */
    static void compute_palette(const glm::mat4& owner_world, const std::vector<glm::mat4>& bone_worlds,
                                const std::vector<uint8_t>& bone_valid, const std::vector<glm::mat4>& inverse_bind,
                                std::vector<glm::mat4>& out) {
        const glm::mat4 inv_owner = glm::inverse(owner_world);
        out.assign(bone_worlds.size(), glm::mat4(1.0f));
        for (size_t i = 0; i < bone_worlds.size(); ++i) {
            if (i < bone_valid.size() && !bone_valid[i]) continue;
            out[i] = inv_owner * bone_worlds[i] * (i < inverse_bind.size() ? inverse_bind[i] : glm::mat4(1.0f));
        }
    }

    /**
     * @brief Conservative object-space bounds of the skinned mesh from the palette alone: each
     *        bone's bind-pose box (the vertices it influences) through its skin matrix, unioned,
     *        plus the box of vertices no bone moves. A skinned vertex is a convex blend of its
     *        bones' transforms of it, so it always lies inside the union. Pure (tests).
     */
    static void palette_bounds(const std::vector<glm::vec3>& bone_min, const std::vector<glm::vec3>& bone_max,
                               const glm::vec3& static_min, const glm::vec3& static_max,
                               const std::vector<glm::mat4>& skin_matrices, glm::vec3& out_min, glm::vec3& out_max) {
        out_min = static_min;
        out_max = static_max;
        for (size_t b = 0; b < bone_min.size() && b < skin_matrices.size(); ++b) {
            if (bone_min[b].x > bone_max[b].x) continue;   // influences no vertex
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 corner((c & 1) ? bone_max[b].x : bone_min[b].x,
                                       (c & 2) ? bone_max[b].y : bone_min[b].y,
                                       (c & 4) ? bone_max[b].z : bone_min[b].z);
                const glm::vec3 p = glm::vec3(skin_matrices[b] * glm::vec4(corner, 1.0f));
                out_min = glm::min(out_min, p);
                out_max = glm::max(out_max, p);
            }
        }
    }

    /**
     * @brief Builds the mesh if needed and skins this frame's vertices: with `gpu`, enqueues a
     *        compute dispatch into the mesh's `frame_slot` vertex buffer (palette and bounds are
     *        the only CPU work); without, skins on the CPU and uploads.
     *
     * No-op until `source_` finishes loading (async), matching ClothRenderer's own
     * "starts drawing one frame later" contract for a dependency not ready at start().
     *
     * @param frame_slot The renderer's current in-flight frame index.
     * @param gpu        The pipeline's skinning pass, or null for the CPU path.
     */
    void upload(uint32_t frame_slot, toy::render::passes::SkinningPass* gpu = nullptr) {
        if (!mesh_ && !ensure_mesh_(gpu)) return;
        update_palette_();
        if (gpu_) {
            glm::vec3 lo, hi;
            palette_bounds(bone_min_, bone_max_, static_min_, static_max_, skin_matrices_, lo, hi);
            toy::render::passes::SkinningPass::Job job;
            job.set          = &gpu_->sets[frame_slot % gpu_->sets.size()];
            job.palette      = &gpu_->palette;
            job.matrices     = skin_matrices_.empty() ? nullptr : skin_matrices_.data();
            job.bone_count   = static_cast<uint32_t>(skin_matrices_.size());
            job.vertex_count = static_cast<uint32_t>(source_->vertices.size());
            job.frame_slot   = frame_slot;
            gpu->enqueue(job);
            mesh_->mark_gpu_written(frame_slot, lo, hi);
            cpu_vertices_dirty_ = true;
        } else {
            skin(*source_, skin_matrices_, vertices_);
            cpu_vertices_dirty_ = false;
            mesh_->update_vertices(vertices_.data(), vertices_.size(), frame_slot);
        }
    }

    /**
     * @brief Drops the GPU mesh so the next upload rebuilds it from the (new) source -- for a tool
     *        replacing the bind pose (the toyeditor editing the mesh). Bones are re-resolved against
     *        the rest pose captured in start(), never the current animated one.
     */
    void rebuild() {
        if (gpu_) device_.wait_idle();   // an in-flight dispatch may still read its buffers
        gpu_.reset();
        mesh_.reset();
        vertices_.clear();
    }

    /** @brief True once the GPU mesh exists and is bound to the sibling MeshRenderer. */
    bool is_ready() const { return mesh_ != nullptr; }
    /** @brief True when the compute pass skins this mesh (false: the CPU fallback). */
    bool gpu_skinned() const { return gpu_ != nullptr; }
    /** @brief The dynamic GPU mesh (null until ready). */
    const std::shared_ptr<Mesh>& mesh() const { return mesh_; }
    /** @brief This frame's skin matrices (palette order). */
    const std::vector<glm::mat4>& skin_matrices() const { return skin_matrices_; }
    /**
     * @brief The last skinned vertices (object space; tests and tools). On the GPU path they
     *        are CPU-skinned on demand from the same palette the dispatch used.
     */
    const std::vector<Vertex>& skinned_vertices() const {
        if (cpu_vertices_dirty_ && source_.is_loaded()) {
            skin(*source_, skin_matrices_, vertices_);
            cpu_vertices_dirty_ = false;
        }
        return vertices_;
    }

private:
    /**
     * @brief Allocates the dynamic GPU mesh from the loaded source's bind pose and binds it
     *        to the sibling MeshRenderer. Idempotent; returns false until `source_` is loaded.
     */
    bool ensure_mesh_(toy::render::passes::SkinningPass* gpu) {
        if (!source_.is_loaded() || !renderer_) return false;
        const SkinnedMeshSource& src = *source_;
        if (src.vertices.empty() || src.indices.empty()) return false;
        resolve_bones_(src);
        build_bone_boxes_(src);

        vertices_ = src.vertices; // bind pose; upload() re-skins
        mesh_ = std::make_shared<Mesh>(
            Mesh::from_arrays(device_, allocator_, vertices_, src.indices, frames_in_flight_,
                              /*compute_writable=*/gpu != nullptr));
        if (gpu) build_gpu_(*gpu, src);

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
    void update_palette_() {
        auto* tc = owner ? owner->get_transform() : nullptr;
        const glm::mat4 owner_world = tc ? tc->get_world_matrix() : glm::mat4(1.0f);
        bone_worlds_.assign(bones_.size(), glm::mat4(1.0f));
        bone_valid_.assign(bones_.size(), 0);
        for (size_t i = 0; i < bones_.size(); ++i) {
            if (!bones_[i]) continue; // unresolved bone -- stays identity, see resolve_bones_()
            auto* bone_tc = bones_[i]->get_transform();
            if (!bone_tc) continue;
            bone_worlds_[i] = bone_tc->get_world_matrix();
            bone_valid_[i] = 1;
        }
        compute_palette(owner_world, bone_worlds_, bone_valid_, inverse_bind_, skin_matrices_);
    }

    /** @brief Per-bone bind-pose boxes of the vertices each bone influences (for palette_bounds()). */
    void build_bone_boxes_(const SkinnedMeshSource& src) {
        const glm::vec3 empty_min(std::numeric_limits<float>::max()), empty_max(std::numeric_limits<float>::lowest());
        bone_min_.assign(bones_.size(), empty_min);
        bone_max_.assign(bones_.size(), empty_max);
        static_min_ = empty_min;
        static_max_ = empty_max;
        for (size_t i = 0; i < src.vertices.size(); ++i) {
            const glm::ivec4 j = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
            const glm::vec4  w = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);
            const glm::vec3& p = src.vertices[i].position;
            bool any = false;
            for (int c = 0; c < 4; ++c) {
                if (j[c] < 0 || w[c] <= 0.0f || static_cast<size_t>(j[c]) >= bones_.size()) continue;
                bone_min_[j[c]] = glm::min(bone_min_[j[c]], p);
                bone_max_[j[c]] = glm::max(bone_max_[j[c]], p);
                any = true;
            }
            if (!any) { static_min_ = glm::min(static_min_, p); static_max_ = glm::max(static_max_, p); }
        }
    }

    /** @brief The compute path's per-mesh data: bind-pose SSBO, palette ring, one set per slot. */
    void build_gpu_(toy::render::passes::SkinningPass& pass, const SkinnedMeshSource& src) {
        using namespace coopa::gfx;
        using BindVertex = toy::render::passes::SkinningPass::BindVertex;
        std::vector<BindVertex> bind(src.vertices.size());
        for (size_t i = 0; i < src.vertices.size(); ++i) {
            const Vertex& v = src.vertices[i];
            bind[i].pos_u   = glm::vec4(v.position, v.uv.x);
            bind[i].nrm_v   = glm::vec4(v.normal, v.uv.y);
            bind[i].tangent = v.tangent;
            bind[i].joints  = (i < src.joints.size())  ? src.joints[i]  : glm::ivec4(-1);
            bind[i].weights = (i < src.weights.size()) ? src.weights[i] : glm::vec4(0.0f);
        }
        const uint64_t bind_bytes = sizeof(BindVertex) * bind.size();
        const uint64_t pal_bytes  = sizeof(glm::mat4) * std::max<size_t>(bones_.size(), 1);
        auto g = std::make_unique<GpuSkin>(
            memory::make_storage_buffer(device_, allocator_, bind_bytes, BufferUsage::None, MemoryResidency::CpuToGpu),
            memory::StorageBufferRing(device_, allocator_, pal_bytes, frames_in_flight_, BufferUsage::None,
                                      MemoryResidency::CpuToGpu),
            pipeline::DescriptorPoolBuilder().add_sets(pass.layout(), frames_in_flight_).build(device_));
        g->bind_pose.upload(bind.data(), bind_bytes);
        const std::vector<glm::mat4> identity(std::max<size_t>(bones_.size(), 1), glm::mat4(1.0f));
        for (uint32_t s = 0; s < frames_in_flight_; ++s) {
            g->palette.upload(s, identity.data(), pal_bytes);
            g->sets.emplace_back(device_, g->pool, pass.layout());
            g->sets.back().bind_storage_buffer(0, g->bind_pose);
            g->sets.back().bind_storage_buffer(1, g->palette.current(s));
            g->sets.back().bind_storage_buffer(2, mesh_->vertex_buffer(s));
        }
        gpu_ = std::move(g);
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

    struct GpuSkin {
        coopa::gfx::memory::Buffer                      bind_pose;
        coopa::gfx::memory::StorageBufferRing           palette;
        coopa::gfx::pipeline::DescriptorPool            pool;
        std::vector<coopa::gfx::pipeline::DescriptorSet> sets;   // per frame slot
        GpuSkin(coopa::gfx::memory::Buffer b, coopa::gfx::memory::StorageBufferRing p,
                coopa::gfx::pipeline::DescriptorPool dp)
            : bind_pose(std::move(b)), palette(std::move(p)), pool(std::move(dp)) {}
        ~GpuSkin() { sets.clear(); }                             // sets free into pool first
    };

    MeshRenderer*             renderer_ = nullptr;
    std::shared_ptr<Mesh>     mesh_;
    std::unique_ptr<GpuSkin>  gpu_;           // declared after mesh_: destroyed first
    mutable std::vector<Vertex> vertices_;
    mutable bool              cpu_vertices_dirty_ = false;
    std::vector<glm::mat4>    skin_matrices_;
    std::vector<glm::mat4>    bone_worlds_;
    std::vector<uint8_t>      bone_valid_;
    std::vector<glm::vec3>    bone_min_, bone_max_;   // bind-pose box per bone (palette_bounds)
    glm::vec3                 static_min_{0.0f}, static_max_{0.0f};
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
