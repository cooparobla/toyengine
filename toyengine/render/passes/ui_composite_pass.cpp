#include <toyengine/render/passes/ui_composite_pass.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

UiCompositePass::UiCompositePass(coopa::gfx::core::Device& device,
                coopa::gfx::pipeline::RenderPass& target_pass,
                const std::string& vert_spv,
                const std::string& frag_spv)
{
    using namespace coopa::gfx;

    vert_shader_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
    frag_shader_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

    desc_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
        pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, ShaderStage::Fragment)   // base: post-processed scene
            .combined_sampler(1, ShaderStage::Fragment)   // ui: world UI layer
            .build(device));

    desc_pool_ = std::make_unique<pipeline::DescriptorPool>(
        pipeline::DescriptorPoolBuilder().add_sets(*desc_layout_, 1).build(device));

    desc_set_ = std::make_unique<pipeline::DescriptorSet>(device, *desc_pool_, *desc_layout_);

    pipeline::PipelineDesc desc;
    desc.shaders = { vert_shader_.get(), frag_shader_.get() };
    desc.descriptor_layouts = { desc_layout_.get() };
    desc.raster.cull = CullMode::None;
    desc.depth.test  = false;
    desc.depth.write = false;
    // No blend state: the composite is done in the shader, not by the blender, because
    // the destination is cleared black every frame and the letterbox bars must stay black.

    pipeline_ = std::make_unique<pipeline::Pipeline>(device, target_pass, desc);
}

void UiCompositePass::draw(coopa::gfx::command::CommandBuffer& cmd, const LetterboxRect& rect) const {
    cmd.bind_pipeline(*pipeline_);
    cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y),
                     static_cast<float>(rect.w), static_cast<float>(rect.h));
    cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
    cmd.bind_descriptor_set(*desc_set_);
    cmd.draw(3); // Fullscreen triangle
}

} // namespace passes
} // namespace render
} // namespace toy
