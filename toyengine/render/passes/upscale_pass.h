/**
 * @file upscale_pass.h
 * @brief Blits the low-resolution LDR buffer into a centred, integer-scaled
 *        sub-rect of the swapchain -- the letterboxed nearest-neighbour
 *        upscale that gives the engine its pixel-art look.
 *
 * Modeled directly on gfxcoopa's PresentPass (gfxcoopa/engine/passes/present_pass.h),
 * but PresentPass::draw() always fills the full swapchain extent with no x/y
 * offset, so it cannot letterbox. UpscalePass adds that offset via
 * pixel_math::LetterboxRect; everything else (single combined-image-sampler
 * descriptor, fullscreen triangle, no depth) is unchanged.
 */

#ifndef TOYENGINE_RENDER_PASSES_UPSCALE_PASS_H
#define TOYENGINE_RENDER_PASSES_UPSCALE_PASS_H

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

#include <toyengine/render/pixel_math.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class UpscalePass
 * @brief Draws the low-resolution source image into a letterboxed viewport
 *        on the swapchain, using a NEAREST sampler so every low-res texel
 *        becomes an exact NxN block of swapchain pixels.
 */
class UpscalePass {
public:
    UpscalePass(coopa::gfx::core::Device& device,
               coopa::gfx::pipeline::RenderPass& swapchain_pass,
               const std::string& vert_spv,
               const std::string& frag_spv)
        : device_(device)
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
            device, swapchain_pass,
            std::vector<coopa::gfx::pipeline::Shader*>{vert_shader_.get(), frag_shader_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{desc_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{});
    }

    UpscalePass(const UpscalePass&) = delete;
    UpscalePass& operator=(const UpscalePass&) = delete;

    /**
     * @brief Binds the low-resolution source image with a NEAREST sampler.
     *
     * Must be called before Renderer::begin_frame() -- DescriptorSet::bind_image
     * calls vkUpdateDescriptorSets immediately, which is unsafe mid-frame.
     */
    void set_source_image(VkImageView view, const coopa::gfx::engine::util::Sampler& nearest_sampler) {
        desc_set_->bind_image(0, view, nearest_sampler.handle());
    }

    /**
     * @brief Draws into the centred, integer-scaled letterbox rect.
     *
     * The swapchain pass's clear (black, by convention) fills the bars
     * outside `rect` -- this call only needs to draw inside it.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, const LetterboxRect& rect) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y),
                         static_cast<float>(rect.w), static_cast<float>(rect.h));
        cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
        cmd.bind_descriptor_set(pipeline_->layout(), *desc_set_, 0);
        cmd.draw(3); // Fullscreen triangle
    }

private:
    coopa::gfx::core::Device& device_;

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

#endif // TOYENGINE_RENDER_PASSES_UPSCALE_PASS_H
