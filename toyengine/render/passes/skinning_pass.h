/**
 * @file skinning_pass.h
 * @brief GPU skinning pre-pass: one compute dispatch per skinned mesh, all sharing one barrier,
 *        writing straight into each mesh's per-frame-slot dynamic vertex buffer.
 *
 * Owned by ToyRenderPipeline (created only when `render.skinning` is "gpu" and the device has
 * compute). Each SkinnedMeshRenderer owns its own GPU data -- the static bind-pose buffer, a
 * per-slot palette ring and one descriptor set per slot against layout() -- and enqueue()s a
 * Job from Engine::upload_dynamic_meshes_(). record() runs at the very start of the frame's
 * command buffer, after the slot's fence wait, so the palette upload it does there can never
 * race the GPU still reading that slot; then it dispatches everything and puts down one
 * compute_to_draw_barrier() before the shadow and G-buffer passes read the vertices.
 *
 * Consumers are untouched: they bind Mesh::bind()'s active slot exactly as for a CPU upload.
 */

#ifndef TOYENGINE_RENDER_PASSES_SKINNING_PASS_H
#define TOYENGINE_RENDER_PASSES_SKINNING_PASS_H

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/memory/storage_buffer.h>

namespace toy {
namespace render {
namespace passes {

class SkinningPass {
public:
    /// One bind-pose vertex as skin.comp reads it (std430, 80 bytes). UV rides in the w lanes.
    struct BindVertex {
        glm::vec4  pos_u;
        glm::vec4  nrm_v;
        glm::vec4  tangent;
        glm::ivec4 joints;
        glm::vec4  weights;
    };
    static_assert(sizeof(BindVertex) == 80, "skin.comp's BindVertex is 80 bytes");

    /// One mesh's dispatch for this frame. Pointers must stay valid until record().
    struct Job {
        const coopa::gfx::pipeline::DescriptorSet* set = nullptr;   ///< this slot's set
        coopa::gfx::memory::StorageBufferRing*     palette = nullptr;
        const glm::mat4*                           matrices = nullptr;
        uint32_t                                   bone_count = 0;
        uint32_t                                   vertex_count = 0;
        uint32_t                                   frame_slot = 0;
    };

    SkinningPass(coopa::gfx::core::Device& device, const std::string& comp_spv)
        : device_(device),
          layout_(coopa::gfx::pipeline::DescriptorLayoutBuilder()
                      .storage_buffer(0, coopa::gfx::ShaderStage::Compute)
                      .storage_buffer(1, coopa::gfx::ShaderStage::Compute)
                      .storage_buffer(2, coopa::gfx::ShaderStage::Compute)
                      .build(device)),
          pipeline_(device, comp_spv, {&layout_},
                    {{coopa::gfx::ShaderStage::Compute, 0, 2 * sizeof(uint32_t)}}) {}

    coopa::gfx::core::Device& device() { return device_; }
    /// The set layout every SkinnedMeshRenderer allocates its per-slot sets against.
    coopa::gfx::pipeline::DescriptorSetLayout& layout() { return layout_; }

    /// Queues one mesh for this frame's record(). Cheap; no GPU work here.
    void enqueue(const Job& job) { if (job.set && job.vertex_count) jobs_.push_back(job); }
    /// Drops queued jobs (Engine calls it before the uploads, so a skipped frame never
    /// leaves stale pointers behind).
    void begin_frame() { jobs_.clear(); }
    size_t pending() const { return jobs_.size(); }

    /**
     * @brief Uploads every queued palette, records one dispatch per mesh and a single
     *        compute_to_draw_barrier(). Outside any render pass. Returns the dispatch count.
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

#endif // TOYENGINE_RENDER_PASSES_SKINNING_PASS_H
