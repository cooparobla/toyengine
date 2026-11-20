/**
 * @file cloth_deform_pass.h
 * @brief Character-cloth deform pre-pass: one compute dispatch per clothed mesh, skinning the
 *        bind pose and blending it toward the CPU-simulated cloth, straight into the mesh's
 *        per-frame-slot dynamic vertex buffer.
 *
 * A structural copy of SkinningPass rather than a mode of it: this pass needs two more storage
 * buffers (the simulated welded vertices, and the render-vertex -> welded-vertex map with its
 * baked blend weight), and SkinningPass's 3-binding layout is load-bearing for every plain
 * skinned mesh. Owned by ToyRenderPipeline under the same gate (`render.skinning` == "gpu" and
 * compute support); toy::cloth::ClothMeshSystem owns the per-garment GPU data and enqueue()s a
 * Job from Engine::upload_dynamic_meshes_(). record() runs right after SkinningPass::record(),
 * at the top of the frame's command buffer, with its own single compute_to_draw_barrier().
 *
 * The LBS half of cloth_deform.comp matches skin.comp's mode 0 exactly; the blend half matches
 * ClothMeshSystem's CPU fallback exactly -- the CPU-vs-GPU parity test in
 * engine/gpu/character_cloth_test.cpp holds both.
 */

#ifndef TOYENGINE_RENDER_PASSES_CLOTH_DEFORM_PASS_H
#define TOYENGINE_RENDER_PASSES_CLOTH_DEFORM_PASS_H

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/memory/storage_buffer.h>

namespace toy {
namespace render {
namespace passes {

class ClothDeformPass {
public:
    /// One simulated welded vertex as cloth_deform.comp reads it (std430, 32 bytes).
    struct SimVertex {
        glm::vec4 pos;  ///< simulated position.xyz, object space
        glm::vec4 nrm;  ///< simulated area-weighted normal.xyz
    };
    static_assert(sizeof(SimVertex) == 32, "cloth_deform.comp's SimVertex is 32 bytes");

    /// One render vertex's route into the sim (std430, 16 bytes). Static per garment.
    struct MapEntry {
        uint32_t welded = 0;      ///< index into the sim buffer
        float sim_weight = 0.0f;  ///< 0 = skinned, 1 = simulated
        glm::vec2 _pad{0.0f};
    };
    static_assert(sizeof(MapEntry) == 16, "cloth_deform.comp's MapEntry is 16 bytes");

    /// One garment's dispatch for this frame. Pointers must stay valid until record().
    struct Job {
        const coopa::gfx::pipeline::DescriptorSet* set = nullptr;      ///< this slot's set
        coopa::gfx::memory::StorageBufferRing*     palette = nullptr;  ///< the renderer's ring
        const glm::mat4*                           matrices = nullptr;
        coopa::gfx::memory::StorageBufferRing*     sim = nullptr;      ///< the garment's ring
        const SimVertex*                           sim_data = nullptr;
        uint32_t                                   bone_count = 0;
        uint32_t                                   vertex_count = 0;   ///< render vertices
        uint32_t                                   welded_count = 0;   ///< sim vertices
        uint32_t                                   frame_slot = 0;
    };

    ClothDeformPass(coopa::gfx::core::Device& device, const std::string& comp_spv)
        : device_(device),
          layout_(coopa::gfx::pipeline::DescriptorLayoutBuilder()
                      .storage_buffer(0, coopa::gfx::ShaderStage::Compute)
                      .storage_buffer(1, coopa::gfx::ShaderStage::Compute)
                      .storage_buffer(2, coopa::gfx::ShaderStage::Compute)
                      .storage_buffer(3, coopa::gfx::ShaderStage::Compute)
                      .storage_buffer(4, coopa::gfx::ShaderStage::Compute)
                      .build(device)),
          pipeline_(device, comp_spv, {&layout_},
                    {{coopa::gfx::ShaderStage::Compute, 0, 3 * sizeof(uint32_t)}}) {}

    coopa::gfx::core::Device& device() { return device_; }
    /// The set layout every garment allocates its per-slot sets against.
    coopa::gfx::pipeline::DescriptorSetLayout& layout() { return layout_; }

    /// Queues one garment for this frame's record(). Cheap; no GPU work here.
    void enqueue(const Job& job) { if (job.set && job.vertex_count) jobs_.push_back(job); }
    /// Drops queued jobs (Engine calls it before the uploads, so a skipped frame never
    /// leaves stale pointers behind).
    void begin_frame() { jobs_.clear(); }
    size_t pending() const { return jobs_.size(); }

    /**
     * @brief Uploads every queued palette and sim buffer, records one dispatch per garment and
     *        a single compute_to_draw_barrier(). Outside any render pass. Returns the count.
     */
    uint32_t record(coopa::gfx::command::CommandBuffer& cmd);

private:
    coopa::gfx::core::Device&                 device_;
    coopa::gfx::pipeline::DescriptorSetLayout layout_;
    coopa::gfx::pipeline::ComputePipeline     pipeline_;
    std::vector<Job>                          jobs_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_CLOTH_DEFORM_PASS_H
