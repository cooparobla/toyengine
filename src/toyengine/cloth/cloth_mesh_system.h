/**
 * @file cloth_mesh_system.h
 * @brief Simulates every CharacterCloth: builds an XPBD cloth from the sibling skinned mesh's
 *        painted channels, skins per-frame targets, gathers colliders, solves, and blends the
 *        result into the renderer's dynamic mesh.
 *
 * **Why order 346, not inside the physics step.** A garment constrained to THIS frame's skinned
 * pose cannot solve at Physics (100): the rig pose is written by SkeletalAnimation (300), IK
 * (319-321), Ragdoll (330) and resolved by RigPose (340). At 346 -- after BoneAttachment (345),
 * before TransformResolve (350) -- the pose is final, `Rig::world_matrix()` and
 * `Transform::get_world_matrix()` both resolve lazily so current values are readable, and the
 * physics world finished stepping at 100, so its broadphase holds the poses this frame draws
 * (`PhysicsWorld::gather_cloth_colliders()` is read-only and safe between steps). Solving here
 * gives ZERO frames of lag between animation and cloth -- the thing that visually separates
 * clothing from Unity's one-frame-stale cloth -- at the cost that these cloths live outside
 * `PhysicsWorld::world_state_hash()`; determinism is instead covered by the solver's
 * serial-vs-parallel bit-identity tests (cloth_solver_test.cpp). Coupling is one-way, exactly
 * like grid cloth: nothing downstream of physics ever reads the result.
 *
 * **State ownership.** Everything derived -- the sim cloth, welded skinning data, bone collider
 * shapes, blend weights -- is owned here, keyed by `CharacterCloth::state_key()` (the owner's
 * name), never by a `Component*`: the editor destroys and recreates components per inspector
 * keystroke (the HairSystem precedent). The rebuild gate is a hash of topology-affecting inputs
 * only; tunables are written through to the live sim every frame.
 *
 * **The render claim.** A live entry claims its sibling SkinnedMeshRenderer's upload:
 * `Engine::upload_dynamic_meshes_()` asks `claims()` and skips the renderer's own upload, then
 * calls `upload()` here, which writes the blended (skinned mixed with simulated, by the painted
 * max-distance fade) vertices into the same dynamic mesh. The sim runs on the WELDED vertices
 * (`SkinnedMeshSource::source_positions` -- the file's raw vertex list is the welded topology,
 * and `source_vertex` maps each unwelded render corner back to it, so no positional welding is
 * ever done here).
 */

#ifndef TOYENGINE_CLOTH_CLOTH_MESH_SYSTEM_H
#define TOYENGINE_CLOTH_CLOTH_MESH_SYSTEM_H

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/scene/scene_system.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <physxcoopa/cloth/cloth.h>
#include <physxcoopa/cloth/cloth_solver.h>
#include <physxcoopa/collision/shape.h>

#include <toyengine/cloth/character_cloth.h>
#include <toyengine/render/passes/cloth_deform_pass.h>

namespace coopa::physx::system { class PhysicsSystem; }
namespace toy::scene { class SkinnedMeshRenderer; }

namespace toy {
namespace cloth {

/// Update order: after RigPose (340) and BoneAttachment (345), before TransformResolve (350) --
/// see this file's doc for the full argument.
inline constexpr int k_cloth_mesh_system_order = 346;

/**
 * @class ClothMeshSystem
 * @brief Owns and advances every CharacterCloth's simulation; see this file's doc.
 */
class ClothMeshSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "ClothMesh"; }

    /// @brief Builds/rebuilds entries, skins targets, gathers colliders and solves.
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /// @brief False: in edit mode the garment shows its plain skinned pose (and Cloth Paint
    ///        needs the undeformed mesh anyway).
    bool runs_in_edit_mode() const override { return false; }

    /**
     * @brief True when this system owns `smr`'s per-frame deform -- a live entry sits on the
     *        same object. `Engine::upload_dynamic_meshes_()` then skips the renderer's own
     *        upload and calls upload() here instead.
     */
    bool claims(const toy::scene::SkinnedMeshRenderer& smr) const;

    /**
     * @brief Writes every claimed renderer's blended vertices for this frame.
     *
     * Same contract as HairSystem::upload(): called from `Engine::upload_dynamic_meshes_()`,
     * after late_update() and before the frame's command buffer is recorded -- the only window
     * where the in-flight slot is known. With `deform` (and a GPU-skinned renderer that is not
     * preserve_volume), one compute Job per garment is enqueued and the CPU uploads nothing but
     * the welded sim buffer; otherwise the blend happens here on the CPU.
     *
     * @param deform    The pipeline's cloth deform pass, or null for the CPU blend.
     * @param allocator Allocates a garment's GPU buffers on first GPU use; may be null when
     *                  `deform` is null.
     */
    void upload(coopa::scene::Scene& scene, std::uint32_t frame_slot,
                toy::render::passes::ClothDeformPass* deform = nullptr,
                coopa::gfx::memory::Allocator* allocator = nullptr);

    /// @brief The live simulation for `key`, or null -- tests and the inspector.
    const coopa::physx::cloth::Cloth* cloth(const std::string& key) const;

    /// @brief Simulated (welded) particle count for `key`, or 0.
    std::size_t particle_count(const std::string& key) const;

    /// @brief Bone colliders resolved for `key` (auto fits plus authored specs, minus dropped
    ///        bones), or 0. The number that says whether a garment is covered by a whole body
    ///        or by the three bones someone remembered -- tests and the inspector.
    std::size_t bone_collider_count(const std::string& key) const;

    /**
     * @brief Every bone collider resolved for `key`, snapshotted at the pose it was last solved
     *        against; empty when `key` has no live entry.
     *
     * What a caller can do with this that bone_collider_count() cannot: compare the shapes the
     * garment actually collides with against the body a viewer SEES. Those are two different
     * models -- a capsule fitted to a bone versus the part mesh riding it -- and a part sticking
     * out past its own collider is cloth the solver was never asked to hold off, which is the
     * one failure that reads on screen as a limb through the garment. cloth_probe_test.cpp
     * asserts the containment; the inspector's overlay draws it.
     *
     * Each ClothCollider::shape aliases this system's storage and is valid only while the entry
     * lives -- the same non-owning contract PhysicsWorld::gather_cloth_colliders() has.
     */
    std::vector<coopa::physx::cloth::ClothCollider> bone_colliders(const std::string& key) const;

private:
    /// Everything derived for one garment. Keyed by state_key(), never by a pointer.
    struct Entry {
        std::uint64_t build_hash = 0;         ///< Topology inputs the build is valid for.
        coopa::physx::cloth::Cloth sim;       ///< Owned here, NOT by PhysicsWorld (see file doc).

        // --- Welded-topology skinning data, fixed at build ---------------------------------
        std::vector<uint32_t> render_to_welded; ///< Per unwelded render vertex (= source_vertex).
        std::vector<glm::ivec4> welded_joints;  ///< Per welded vertex, first corner's influences.
        std::vector<glm::vec4>  welded_weights;
        std::vector<glm::vec3>  welded_bind;    ///< Bind pose, object space.
        std::vector<glm::vec3>  welded_bind_normal;
        std::vector<float>      render_weight;  ///< Per render vertex: the skin->sim blend fade.

        // --- Per-frame outputs -------------------------------------------------------------
        std::vector<glm::vec3> obj_positions;  ///< Post-solve particle positions, object space.
        std::vector<glm::vec3> obj_normals;    ///< Area-weighted welded normals, object space.
        glm::vec3 bounds_min{0.0f}, bounds_max{0.0f}; ///< Conservative object-space bounds.

        // --- Bone colliders ----------------------------------------------------------------
        std::vector<coopa::physx::collision::Shape> bone_shapes; ///< Persistent; offset baked in.
        std::vector<int> bone_index;                             ///< Rig bone per shape, -1 = none.
        /// Each bone collider's pose at the PREVIOUS solve, so this frame's gather can hand the
        /// solver the volume the limb swept between the two -- see gather_colliders_().
        std::vector<glm::vec3> prev_bone_position;
        std::vector<glm::quat> prev_bone_orientation;

        bool live = false;    ///< Built and simulating (claims the renderer).
        bool warned = false;  ///< One-shot "no cloth paint" / "no rig" log.
        std::uint64_t stamp = 0; ///< Last frame this component was seen (reconcile).

        std::vector<coopa::gfx::engine::data::Vertex> blend_scratch; ///< CPU deform scratch.

        /// The GPU deform path's per-garment data: the welded sim ring, the static
        /// render->welded map, and one descriptor set per frame slot against the pass's
        /// layout (bindings 0/1 alias the renderer's own bind-pose buffer and palette ring).
        struct GpuDeform {
            coopa::gfx::core::Device* device = nullptr;
            const void* mesh_id = nullptr; ///< The Mesh the sets bind; rebuild when it changes.
            coopa::gfx::memory::StorageBufferRing sim;
            coopa::gfx::memory::Buffer map;
            coopa::gfx::pipeline::DescriptorPool pool;
            std::vector<coopa::gfx::pipeline::DescriptorSet> sets; ///< per frame slot
            GpuDeform(coopa::gfx::core::Device& d, coopa::gfx::memory::StorageBufferRing s,
                      coopa::gfx::memory::Buffer m, coopa::gfx::pipeline::DescriptorPool p)
                : device(&d), sim(std::move(s)), map(std::move(m)), pool(std::move(p)) {}
            /// An in-flight dispatch may still read these buffers: drain first (destroyed on
            /// topology rebuilds and entry teardown -- rare, so a stall is acceptable).
            ~GpuDeform() {
                if (device) device->wait_idle();
                sets.clear(); // sets free into pool first
            }
        };
        std::unique_ptr<GpuDeform> gpu;
        std::vector<toy::render::passes::ClothDeformPass::SimVertex> sim_upload;
    };

    /// @brief (Re)builds `e` from the component + loaded source; returns false when inputs are
    ///        not ready (source still loading, no rig, no cloth paint).
    bool build_entry_(Entry& e, CharacterCloth& cc, toy::scene::SkinnedMeshRenderer& smr);

    /// @brief Skins this frame's welded targets (position + normal, world space) into `e.sim`.
    void skin_targets_(Entry& e, toy::scene::SkinnedMeshRenderer& smr, const glm::mat4& owner_world,
                       coopa::job::JobEngine* jobs);

    /// @brief Snapshots world + bone colliders for this solve into `colliders_`.
    void gather_colliders_(Entry& e, CharacterCloth& cc, toy::scene::SkinnedMeshRenderer& smr,
                           coopa::physx::system::PhysicsSystem* physics, float dt,
                           bool* any_moving);

    std::unordered_map<std::string, Entry> entries_;
    coopa::physx::cloth::ClothSolverScratch scratch_;
    std::vector<coopa::physx::cloth::ClothCollider> colliders_;
    std::uint64_t frame_stamp_ = 0;
};

/** @brief Installs the system at k_cloth_mesh_system_order (idempotent). */
ClothMeshSystem* install_cloth_mesh_system(coopa::scene::Scene& scene);

} // namespace cloth
} // namespace toy

#endif // TOYENGINE_CLOTH_CLOTH_MESH_SYSTEM_H
