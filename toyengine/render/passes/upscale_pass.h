/**
 * @file upscale_pass.h
 * @brief Blits the low-resolution LDR buffer into a centred sub-rect of the
 *        swapchain -- the nearest-neighbour upscale that gives the engine
 *        its pixel-art look.
 *
 * The destination rect (toy_render_math::LetterboxRect, from compute_display_rect())
 * is either integer-scaled (config.upscale_mode == "integer": crisp NxN texel
 * blocks, letterboxed on every mismatched axis) or a fractional aspect-preserving
 * best fit (the default, "fit": fills the window as closely as the render aspect
 * allows, letterboxing only the one axis that doesn't match) -- UpscalePass itself
 * doesn't care which; it just draws into whatever rect it's given.
 *
 * Modeled directly on gfxcoopa's PresentPass (gfxcoopa/engine/passes/present_pass.h),
 * but PresentPass::draw() always fills the full swapchain extent with no x/y
 * offset, so it cannot letterbox. UpscalePass adds that offset via
 * toy_render_math::LetterboxRect; everything else (single combined-image-sampler
 * descriptor, fullscreen triangle, no depth) is unchanged.
 */

#ifndef TOYENGINE_RENDER_PASSES_UPSCALE_PASS_H
#define TOYENGINE_RENDER_PASSES_UPSCALE_PASS_H

#include <memory>
#include <string>

#include <gfxcoopa/command/command_buffer.h>

#include <toyengine/render/toy_render_math.h>

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
               const std::string& frag_spv);

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
     * @brief Draws into the centred destination rect.
     *
     * The swapchain pass's clear (black, by convention) fills the bars
     * outside `rect` -- this call only needs to draw inside it.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, const LetterboxRect& rect) const;

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

#endif // TOYENGINE_RENDER_PASSES_UPSCALE_PASS_H
