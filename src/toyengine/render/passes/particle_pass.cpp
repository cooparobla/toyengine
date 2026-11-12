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
             coopa::gfx::RenderPassRef shared_render_pass,
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
    // Two colour attachments: particle quads draw inside TransparentPass::draw_render_pass(),
    // whose second attachment is the transparent-coverage mask. particle.frag writes no output
    // for it -- NoWrite leaves the coverage the water and glass wrote in place instead of
    // leaving every pixel a quad covered UNDEFINED (see TransparentPass's file doc).
    desc.blend.color_attachment_count = 2;
    if (device.supports_independent_blend()) {   // see TransparentPass::create_pipeline_
        desc.blend.attachment_modes = { pipeline::BlendMode::PremultipliedAlpha,
                                        pipeline::BlendMode::NoWrite };
    }
    desc.descriptor_layouts = {&camera_layout, &light_layout, &hiz_layout, &material_layout, &shadow_layout};
    desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(PushConstants)}};
    pipeline_ = std::make_unique<pipeline::Pipeline>(device, shared_render_pass, desc);
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

void ParticlePass::build_reactive(coopa::gfx::RenderPassRef mask_render_pass) {
    coopa::gfx::pipeline::PipelineDesc d = desc_;
    // desc_ is built for TransparentPass::draw_render_pass(), which carries the coverage
    // attachment beside the colour one. The reactive mask is a single-attachment pass of its
    // own, and a pipeline's blend state must match the render pass it is built against.
    d.blend.color_attachment_count = 1;
    d.blend.attachment_modes.clear();
    d.depth.test = false;
    d.depth.write = false;
    d.blend.mode = coopa::gfx::pipeline::BlendMode::Additive;
    reactive_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
        device_, mask_render_pass, d);
}

void ParticlePass::draw(coopa::gfx::command::CommandBuffer& cmd, size_t batch, uint32_t count, const PushConstants& pc) const {
    if (count == 0 || batch >= firsts_.size()) return;
    if (gpu_ids_[batch]) {
        if (!gpu_) return;
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
        gpu_->draw(cmd, gpu_ids_[batch]);
        return;
    }
    cmd.bind_vertex_buffer(*buffers_[frame_slot_],
                           sizeof(ParticleInstance) * static_cast<uint64_t>(firsts_[batch]), 0);
    cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
    cmd.draw(6, 0, count);
}

std::unique_ptr<coopa::gfx::memory::Buffer> ParticlePass::make_buffer_(uint32_t capacity) {
    return std::make_unique<coopa::gfx::memory::Buffer>(
        device_, allocator_, sizeof(ParticleInstance) * static_cast<uint64_t>(std::max(capacity, 1u)),
        coopa::gfx::BufferUsage::Vertex, coopa::gfx::MemoryResidency::CpuToGpu);
}

} // namespace passes
} // namespace render
} // namespace toy
