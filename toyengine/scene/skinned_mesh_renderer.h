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


#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

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
    void start() override;

    /** @brief The rig root bones are resolved under (after start()). */
    coopa::scene::SceneObject* rig() const { return rig_; }
    /** @brief The resolved palette (after the mesh loaded); entries may be null. */
    const std::vector<coopa::scene::SceneObject*>& bones() const { return bones_; }

    /**
     * @brief Skins `src`'s bind-pose vertices with one matrix per palette entry into `out`
     *        (object space of the mesh's owner). A vertex with no usable influence keeps its
     *        bind pose. Pure; the per-frame upload's whole CPU cost.
     */
    static void skin(const SkinnedMeshSource& src, const std::vector<glm::mat4>& skin_matrices, std::vector<Vertex>& out);

    /**
     * @brief Per bone: inverse(owner_world) * bone_world * inverse_bind -- the skin matrix that
     *        takes a bind-pose vertex (owner object space) to its posed position, again in the
     *        owner's object space. A null bone stays identity. Pure (tests).
     */
    static void compute_palette(const glm::mat4& owner_world, const std::vector<glm::mat4>& bone_worlds,
                                const std::vector<uint8_t>& bone_valid, const std::vector<glm::mat4>& inverse_bind,
                                std::vector<glm::mat4>& out);

    /**
     * @brief Conservative object-space bounds of the skinned mesh from the palette alone: each
     *        bone's bind-pose box (the vertices it influences) through its skin matrix, unioned,
     *        plus the box of vertices no bone moves. A skinned vertex is a convex blend of its
     *        bones' transforms of it, so it always lies inside the union. Pure (tests).
     */
    static void palette_bounds(const std::vector<glm::vec3>& bone_min, const std::vector<glm::vec3>& bone_max,
                               const glm::vec3& static_min, const glm::vec3& static_max,
                               const std::vector<glm::mat4>& skin_matrices, glm::vec3& out_min, glm::vec3& out_max);

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
    void upload(uint32_t frame_slot, toy::render::passes::SkinningPass* gpu = nullptr);

    /**
     * @brief Drops the GPU mesh so the next upload rebuilds it from the (new) source -- for a tool
     *        replacing the bind pose (the toyeditor editing the mesh). Bones are re-resolved against
     *        the rest pose captured in start(), never the current animated one.
     */
    void rebuild();

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
    const std::vector<Vertex>& skinned_vertices() const;

private:
    /**
     * @brief Allocates the dynamic GPU mesh from the loaded source's bind pose and binds it
     *        to the sibling MeshRenderer. Idempotent; returns false until `source_` is loaded.
     */
    bool ensure_mesh_(toy::render::passes::SkinningPass* gpu);

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
    void update_palette_();

    /** @brief Per-bone bind-pose boxes of the vertices each bone influences (for palette_bounds()). */
    void build_bone_boxes_(const SkinnedMeshSource& src);

    /** @brief The compute path's per-mesh data: bind-pose SSBO, palette ring, one set per slot. */
    void build_gpu_(toy::render::passes::SkinningPass& pass, const SkinnedMeshSource& src);

    /** @brief The rig root: `rig:`, else the nearest Animator up the hierarchy, else the topmost ancestor. */
    coopa::scene::SceneObject* find_rig_() const;

    void snapshot_rest_(coopa::scene::SceneObject* o);

    /** @brief A bone path under the rig: "A/B" walks down from the rig root; a bare name may sit at any depth. */
    coopa::scene::SceneObject* find_bone_(const std::string& path) const;

    /** @brief Resolves the palette and its inverse bind matrices (file, else rest pose). */
    void resolve_bones_(const SkinnedMeshSource& src);

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
