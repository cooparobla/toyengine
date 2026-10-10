#include <toyengine/render/passes/particle_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/vertex_layout.h>
#include <toyengine/render/particle_types.h>

namespace toy {
namespace render {
namespace passes {

ParticlePass::ParticlePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
             VkRenderPass shared_render_pass,
             const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
             const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
             const coopa::gfx::pipeline::DescriptorSetLayout& hiz_layout,
             const coopa::gfx::pipeline::DescriptorSetLayout& material_layout,
             const coopa::gfx::pipeline::DescriptorSetLayout& shadow_layout,
             const std::string& vert_spv, const std::string& frag_spv,
             uint32_t initial_capacity)
    : device_(device), allocator_(allocator)
{
    using namespace coopa::gfx;
    vert_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
    frag_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

    pipeline::PipelineDesc desc;
    desc.shaders = {vert_.get(), frag_.get()};
    desc.vertex = VertexLayout{}
        .binding(0, sizeof(ParticleInstance), VertexRate::Instance)
        .attribute(0, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, pos_size)))
        .attribute(1, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, color)))
        .attribute(2, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, velocity_rot)))
        .attribute(3, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, orient)))
        .attribute(4, Format::RGBA32_Sfloat, static_cast<uint32_t>(offsetof(ParticleInstance, misc)));
    desc.raster.cull = CullMode::None;      // quads are seen from both sides (aligned / horizontal)
    desc.depth.test = true;                 // occluded by opaque geometry...
    desc.depth.write = false;               // ...but never by each other (they are sorted)
    desc.depth.compare = CompareOp::Less;
    desc.blend.mode = pipeline::BlendMode::PremultipliedAlpha;
    desc.blend.color_attachment_count = 1;
    desc.descriptor_layouts = {&camera_layout, &light_layout, &hiz_layout, &material_layout, &shadow_layout};
    desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(PushConstants)}};
    pipeline_ = std::make_unique<pipeline::Pipeline>(device, detail::RawRenderPass{shared_render_pass}, desc);
    desc_ = desc;

    for (uint32_t i = 0; i < kFrames; ++i) {
        capacity_[i] = std::max(1u, initial_capacity);
        buffers_[i] = make_buffer_(capacity_[i]);
    }
}

void ParticlePass::upload(uint32_t frame_slot, const std::vector<ParticleDrawBatch>& batches) {
    frame_slot_ = frame_slot;
    firsts_.assign(batches.size(), 0);
    gpu_ids_.assign(batches.size(), 0);
    gpu_batches_ = 0;
    uint32_t total = 0;
    for (size_t i = 0; i < batches.size(); ++i) {
        firsts_[i] = total;
        if (batches[i].gpu_id) {   // GPU-simulated: drawn from GpuParticlePass's buffers
            gpu_ids_[i] = batches[i].gpu_id;
            ++gpu_batches_;
            continue;
        }
        total += batches[i].count;
    }
    total_ = total;
    if (total == 0) return;
    if (total > capacity_[frame_slot]) {
        uint32_t cap = capacity_[frame_slot];
        while (cap < total) cap *= 2;
        buffers_[frame_slot] = make_buffer_(cap);
        capacity_[frame_slot] = cap;
    }
    for (size_t i = 0; i < batches.size(); ++i) {
        if (batches[i].count == 0 || batches[i].gpu_id) continue;
        buffers_[frame_slot]->upload(batches[i].instances, sizeof(ParticleInstance) * batches[i].count,
                                     sizeof(ParticleInstance) * firsts_[i]);
    }
}

void ParticlePass::build_reactive(VkRenderPass mask_render_pass) {
    coopa::gfx::pipeline::PipelineDesc d = desc_;
    d.depth.test = false;
    d.depth.write = false;
    d.blend.mode = coopa::gfx::pipeline::BlendMode::Additive;
    reactive_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
        device_, coopa::gfx::detail::RawRenderPass{mask_render_pass}, d);
}

void ParticlePass::draw(coopa::gfx::command::CommandBuffer& cmd, size_t batch, uint32_t count, const PushConstants& pc) const {
    if (count == 0 || batch >= firsts_.size()) return;
    if (gpu_ids_[batch]) {
        if (!gpu_) return;
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
        gpu_->draw(cmd, gpu_ids_[batch]);
        return;
    }
    cmd.bind_vertex_buffer(*buffers_[frame_slot_], sizeof(ParticleInstance) * static_cast<VkDeviceSize>(firsts_[batch]), 0);
    cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
    cmd.draw(6, 0, count);
}

std::unique_ptr<coopa::gfx::memory::Buffer> ParticlePass::make_buffer_(uint32_t capacity) {
    return std::make_unique<coopa::gfx::memory::Buffer>(
        device_, allocator_, sizeof(ParticleInstance) * static_cast<VkDeviceSize>(std::max(capacity, 1u)),
        coopa::gfx::BufferUsage::Vertex, coopa::gfx::MemoryResidency::CpuToGpu);
}

} // namespace passes
} // namespace render
} // namespace toy
