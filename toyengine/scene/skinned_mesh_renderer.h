/**
 * @file skinned_mesh_renderer.h
 * @brief Skins a bind-pose SkinnedMeshSource against a coopa::rig::Rig's pose every frame
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
 * calls upload() once per frame, after Scene::late_update() (so a coopa::rig::RigPoseSystem's
 * resolved model matrices are current) and before the frame's command buffer is recorded.
 *
 * A skinned mesh's bones are entries of a coopa::rig::Rig, not SceneObjects -- the rig is the
 * `rig:` path if given, else the nearest ancestor (or the owner itself) carrying a Rig
 * component. A bone entry is a bone NAME in that rig's RigAsset, looked up once per name
 * (there is no path-walking: a Rig has no hierarchy below it for a bone name to walk through).
 *
 * The palette comes from either source:
 *  - `bones:` in the mesh's `joints:` / `inverse_bind_matrices:` order (an exporter's), or
 *  - with no `bones:` and a mesh whose vertex groups name the bones (`weights:` -- Blender's
 *    export, and the toyeditor's Weight Paint), the groups themselves: group "Forearm" binds
 *    to the rig bone named "Forearm".
 * Inverse bind matrices come from the file when it has them; otherwise from
 * `Rig::inverse_bind()`, derived from the rig asset's own rest pose -- so a mesh painted in
 * the editor needs no export step at all, and binding no longer depends on scene-load order
 * (there is nothing left to snapshot: the rig's rest pose is asset data, not a transform
 * captured the moment start() happened to run).
 *
 * `preserve_volume` picks how the bones are blended, Blender's option of the same name. Off
 * (the default), the skin matrices are averaged directly -- linear blend skinning: one matrix
 * multiply per influence, but a joint twisted far enough pinches the skin to nothing, the
 * "candy wrapper". On, each matrix is split into a dual quaternion and a scale
 * and the ROTATIONS are blended on the unit sphere instead, which keeps a limb's cross section
 * through a twist. Both paths exist on the CPU and in skin.comp and must agree; the
 * dual-quaternion palette is packed into the same mat4 slot the matrix palette uses (see
 * compute_dq_palette()), so the buffers, the descriptor layout and the upload are untouched
 * and a push constant is all that selects the path.
 */

#ifndef TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
#define TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H

#include <glm/glm.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>


#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <coopa/rig/rig_component.h>

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
 *   bones: [pelvis, spine, chest]   # or omit: the mesh's vertex groups name them
 *   rig: Character                  # optional; default: the nearest ancestor with a Rig
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

    /** @brief Sets the bone palette: names in the rig's RigAsset, in mesh joint-index order (empty: the mesh's vertex groups). */
    void set_bones(std::vector<std::string> bones) { bone_paths_ = std::move(bones); }
    /** @brief Names the rig root explicitly (a scene path); empty = the nearest ancestor carrying a Rig component. */
    void set_rig(std::string rig) { rig_path_ = std::move(rig); }
    /** @brief Blends the bones as dual quaternions instead of matrices -- see this file's doc. */
    void set_preserve_volume(bool on) { preserve_volume_ = on; }
    bool preserve_volume() const { return preserve_volume_; }

    /**
     * @brief Finds the sibling MeshRenderer and the Rig this mesh skins against. Bones
     *        themselves are resolved once the mesh source has loaded (ensure_mesh_()), since
     *        with no `bones:` they are named by the mesh's vertex groups.
     */
    void start() override;

    /** @brief The Rig bones are resolved against (after start()), or null if none was found. */
    coopa::rig::Rig* rig() const { return rig_; }
    /** @brief The resolved palette (after the mesh loaded): an index into rig()'s bones per mesh joint slot, or -1. */
    const std::vector<int>& bone_indices() const { return bone_indices_; }

    /**
     * @brief Skins `src`'s bind-pose vertices with one matrix per palette entry into `out`
     *        (object space of the mesh's owner). A vertex with no usable influence keeps its
     *        bind pose. Pure; the per-frame upload's whole CPU cost.
     *
     * @param preserve_volume Blends dual quaternions derived from `skin_matrices` rather than
     *                        the matrices themselves. Matches skin.comp's mode 1 exactly.
     */
    static void skin(const SkinnedMeshSource& src, const std::vector<glm::mat4>& skin_matrices,
                     std::vector<Vertex>& out, bool preserve_volume = false);

    /**
     * @brief Each skin matrix as a dual quaternion plus a scale, packed into a mat4 so it rides
     *        in the matrix palette's own slot.
     *
     * Column 0 is the rigid rotation as a quaternion (x, y, z, w), column 1 the dual part
     * (0.5 * translation * rotation, same order), column 2 the per-axis scale the rotation was
     * divided out of, column 3 unused. A mirrored bone (negative determinant) keeps its
     * reflection in the scale, where it cannot corrupt the quaternion. Pure (tests, skin.comp).
     */
    static void compute_dq_palette(const std::vector<glm::mat4>& skin_matrices, std::vector<glm::mat4>& out);

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
                               const std::vector<glm::mat4>& skin_matrices, glm::vec3& out_min, glm::vec3& out_max,
                               bool preserve_volume = false);

    /**
     * @brief How much palette_bounds() grows its box under `preserve_volume`, as a fraction of
     *        the box's own diagonal.
     *
     * A dual-quaternion blend is not a convex combination of the bones' transforms of a vertex:
     * between two bones it follows the shorter arc around their screw axis, which bows outside
     * the straight line a matrix blend takes, so the union of the bones' boxes does not contain
     * it. Both ends of that arc ARE in the union, so its chord is at most the union's diagonal,
     * and an arc's sagitta is at most half its chord -- half the diagonal therefore covers any
     * two-bone blend. A loose culling box only costs a draw; a tight one that clips pops a
     * character off the screen.
     */
    static constexpr float k_dq_bounds_margin = 0.5f;

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
    /**
     * @brief The bind-pose source, or null before it loads.
     *
     * Anything that has to grow something ON this skin needs the same triangles the skinning
     * uses -- toy::hair::HairSystem roots hair in them. The mesh FILE is not interchangeable with
     * this: Mesh::build_cpu() welds and reorders its vertices, so its triangle indices do not
     * correspond to these at all.
     */
    const SkinnedMeshSource* source() const { return source_.get(); }
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

    /** @brief The rig this mesh skins against: `rig:` if given, else the nearest ancestor (or the owner) carrying a Rig. */
    coopa::rig::Rig* find_rig_() const;

    /** @brief Resolves the palette (bone indices into rig_'s RigAsset) and its inverse bind matrices (file, else Rig::inverse_bind()). */
    void resolve_bones_(const SkinnedMeshSource& src);

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;
    uint32_t                       frames_in_flight_;

    coopa::asset::AssetHandle<SkinnedMeshSource> source_;
    std::vector<std::string> bone_paths_;
    std::string rig_path_;
    coopa::rig::Rig* rig_ = nullptr;
    std::vector<int> bone_indices_;       // resolved when the source loads; -1 = unresolved
    std::vector<glm::mat4> inverse_bind_; // parallel to bone_indices_

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
    bool                      preserve_volume_ = false;
    std::vector<glm::mat4>    skin_matrices_;
    std::vector<glm::mat4>    dq_palette_;   // preserve_volume: what the GPU palette carries instead

    std::vector<glm::mat4>    bone_worlds_;
    std::vector<uint8_t>      bone_valid_;
    std::vector<glm::vec3>    bone_min_, bone_max_;   // bind-pose box per bone (palette_bounds)
    glm::vec3                 static_min_{0.0f}, static_max_{0.0f};
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_SKINNED_MESH_RENDERER_H
