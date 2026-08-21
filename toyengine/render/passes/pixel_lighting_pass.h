/**
 * @file pixel_lighting_pass.h
 * @brief Banded-PBR deferred lighting pass for the pixel-art pipeline.
 *
 * A stripped fork of gfxcoopa's DeferredLightingPass
 * (gfxcoopa/engine/passes/deferred_lighting_pass.h), not a reuse of it, for
 * two reasons: that class has a latent descriptor-set-index bug when built
 * without GI (its ctor only appends the GI layout at set index 3 when
 * gi_layout != VK_NULL_HANDLE, but draw() unconditionally binds the
 * G-buffer set at index 4 regardless -- with no GI the G-buffer set actually
 * lives at index 3, so that bind is wrong), and toyengine has neither GI nor
 * SSAO to plumb through in the first place. Set layout here is fixed at
 * 0=camera, 1=light, 2=shadow (dir + one point cube map), 3=gbuffer.
 */

#ifndef TOYENGINE_RENDER_PASSES_PIXEL_LIGHTING_PASS_H
#define TOYENGINE_RENDER_PASSES_PIXEL_LIGHTING_PASS_H

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

class PixelLightingPass {
public:
    /** @brief Matches pixel_lighting.frag's PixelParams push constant block (16 bytes). */
    struct PushConstants {
        float light_bands    = 4.0f;
        float spec_threshold = 0.55f;
        float rim_strength   = 0.0f;
        float _pad           = 0.0f;
    };

    PixelLightingPass(coopa::gfx::core::Device& device,
                      coopa::gfx::pipeline::RenderPass& target_pass,
                      VkDescriptorSetLayout camera_layout,
                      VkDescriptorSetLayout light_layout,
                      VkDescriptorSetLayout shadow_layout,
                      const std::string& vert_spv,
                      const std::string& frag_spv)
        : device_(device)
    {
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        std::vector<VkDescriptorSetLayoutBinding> gbuffer_bindings;
        for (uint32_t i = 0; i < 3; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding         = i;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            gbuffer_bindings.push_back(b);
        }
        gbuffer_desc_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, gbuffer_bindings);
        gbuffer_desc_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 1, std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}});
        gbuffer_desc_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *gbuffer_desc_pool_, *gbuffer_desc_layout_);

        coopa::gfx::pipeline::PipelineConfig cfg{};
        cfg.cull_mode   = VK_CULL_MODE_NONE;
        cfg.depth_test  = false;
        cfg.depth_write = false;

        VkPushConstantRange pc_range{};
        pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc_range.offset     = 0;
        pc_range.size       = sizeof(PushConstants);

        pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, target_pass,
            std::vector<coopa::gfx::pipeline::Shader*>{vert_shader_.get(), frag_shader_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{camera_layout, light_layout, shadow_layout, gbuffer_desc_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{pc_range});
    }

    PixelLightingPass(const PixelLightingPass&) = delete;
    PixelLightingPass& operator=(const PixelLightingPass&) = delete;

    void set_gbuffer_images(VkImageView g0_view, VkImageView g1_view, VkImageView g2_view,
                            const coopa::gfx::engine::util::Sampler& linear_sampler) {
        gbuffer_desc_set_->bind_image(0, g0_view, linear_sampler.handle());
        gbuffer_desc_set_->bind_image(1, g1_view, linear_sampler.handle());
        gbuffer_desc_set_->bind_image(2, g2_view, linear_sampler.handle());
    }

    void draw(coopa::gfx::command::CommandBuffer& cmd,
              const coopa::gfx::pipeline::DescriptorSet& camera_set,
              const coopa::gfx::pipeline::DescriptorSet& light_set,
              const coopa::gfx::pipeline::DescriptorSet& shadow_set,
              const PushConstants& params,
              uint32_t viewport_w, uint32_t viewport_h) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(viewport_w), static_cast<float>(viewport_h));
        cmd.set_scissor(0, 0, viewport_w, viewport_h);

        cmd.bind_descriptor_set(pipeline_->layout(), camera_set, 0);
        cmd.bind_descriptor_set(pipeline_->layout(), light_set, 1);
        cmd.bind_descriptor_set(pipeline_->layout(), shadow_set, 2);
        cmd.bind_descriptor_set(pipeline_->layout(), *gbuffer_desc_set_, 3);
        cmd.push_constants(pipeline_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &params);

        cmd.draw(3);
    }

    VkPipelineLayout layout() const { return pipeline_->layout(); }

private:
    coopa::gfx::core::Device& device_;

    std::unique_ptr<coopa::gfx::pipeline::Shader>              vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>              frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> gbuffer_desc_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      gbuffer_desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       gbuffer_desc_set_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline>            pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_PIXEL_LIGHTING_PASS_H
