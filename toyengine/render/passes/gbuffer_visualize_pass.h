/**
 * @file gbuffer_visualize_pass.h
 * @brief Temporary Phase-3 milestone pass: draws the G-buffer's albedo
 *        attachment directly into the low-res LDR target with no lighting.
 *
 * Superseded by the deferred lighting pass (gfxcoopa's DeferredLightingPass,
 * as of the gfxcoopa backbone refactor) in Phase 4 -- kept afterward as a
 * standalone G-buffer diagnostic (same shape as that pass's set-3 G-buffer
 * binding, minus the camera/light/shadow sets).
 */

#ifndef TOYENGINE_RENDER_PASSES_GBUFFER_VISUALIZE_PASS_H
#define TOYENGINE_RENDER_PASSES_GBUFFER_VISUALIZE_PASS_H

#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

class GBufferVisualizePass {
public:
    GBufferVisualizePass(coopa::gfx::core::Device& device,
                         coopa::gfx::pipeline::RenderPass& target_pass,
                         const std::string& vert_spv,
                         const std::string& frag_spv)
    {
        using namespace coopa::gfx;

        vert_shader_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
        frag_shader_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

        desc_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, ShaderStage::Fragment)
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

        pipeline_ = std::make_unique<pipeline::Pipeline>(device, target_pass, desc);
    }

    GBufferVisualizePass(const GBufferVisualizePass&) = delete;
    GBufferVisualizePass& operator=(const GBufferVisualizePass&) = delete;

    void set_albedo_image(coopa::gfx::TextureView view, const coopa::gfx::engine::util::Sampler& sampler) {
        desc_set_->bind_image(0, view, sampler);
    }

    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(viewport_w), static_cast<float>(viewport_h));
        cmd.set_scissor(0, 0, viewport_w, viewport_h);
        cmd.bind_descriptor_set(*desc_set_);
        cmd.draw(3);
    }

private:
    std::unique_ptr<coopa::gfx::pipeline::Shader>              vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>              frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> desc_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       desc_set_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline>            pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_GBUFFER_VISUALIZE_PASS_H
