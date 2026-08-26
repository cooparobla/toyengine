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

        pipeline_ = std::make_unique<pipeline::Pipeline>(device, swapchain_pass, desc);
    }

    UpscalePass(const UpscalePass&) = delete;
    UpscalePass& operator=(const UpscalePass&) = delete;

    /**
     * @brief Binds the low-resolution source image with a NEAREST sampler.
     *
     * Must be called before Renderer::begin_frame() -- DescriptorSet::bind_image
     * calls vkUpdateDescriptorSets immediately, which is unsafe mid-frame.
     */
    void set_source_image(coopa::gfx::TextureView view, const coopa::gfx::engine::util::Sampler& nearest_sampler) {
        desc_set_->bind_image(0, view, nearest_sampler);
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
        cmd.bind_descriptor_set(*desc_set_);
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
