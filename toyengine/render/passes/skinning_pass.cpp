#include <toyengine/render/passes/skinning_pass.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/types/enums.h>

namespace toy {
namespace render {
namespace passes {

uint32_t SkinningPass::record(coopa::gfx::command::CommandBuffer& cmd) {
    if (jobs_.empty()) return 0;
    cmd.bind_pipeline(pipeline_);
    for (const Job& j : jobs_) {
        if (j.palette && j.matrices && j.bone_count) {
            j.palette->upload(j.frame_slot, j.matrices, sizeof(glm::mat4) * j.bone_count);
        }
        const uint32_t push[2] = {j.vertex_count, j.bone_count};
        cmd.bind_descriptor_set(*j.set);
        cmd.push_constants(coopa::gfx::ShaderStage::Compute, 0, sizeof(push), push);
        cmd.dispatch(coopa::gfx::pipeline::ComputePipeline::groups_for(j.vertex_count, 64));
    }
    cmd.compute_to_draw_barrier();
    const uint32_t n = static_cast<uint32_t>(jobs_.size());
    jobs_.clear();
    return n;
}

} // namespace passes
} // namespace render
} // namespace toy
