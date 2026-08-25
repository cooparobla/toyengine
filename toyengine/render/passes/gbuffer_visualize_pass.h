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

#include <volk/volk.h>
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
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        desc_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            device, std::vector<VkDescriptorSetLayoutBinding>{binding});
        desc_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 1,
            std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}});
        desc_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *desc_layout_);

        coopa::gfx::pipeline::PipelineConfig cfg{};
        cfg.cull_mode   = VK_CULL_MODE_NONE;
        cfg.depth_test  = false;
        cfg.depth_write = false;

        pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, target_pass,
            std::vector<coopa::gfx::pipeline::Shader*>{vert_shader_.get(), frag_shader_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{desc_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{});
    }

    GBufferVisualizePass(const GBufferVisualizePass&) = delete;
    GBufferVisualizePass& operator=(const GBufferVisualizePass&) = delete;

    void set_albedo_image(VkImageView view, const coopa::gfx::engine::util::Sampler& sampler) {
        desc_set_->bind_image(0, view, sampler.handle());
    }

    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(viewport_w), static_cast<float>(viewport_h));
        cmd.set_scissor(0, 0, viewport_w, viewport_h);
        cmd.bind_descriptor_set(pipeline_->layout(), *desc_set_, 0);
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
