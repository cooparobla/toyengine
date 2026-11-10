/**
 * @file present_pass.h
 * @brief Blits the final post-processed LDR color target to the swapchain.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_PRESENT_PASS_H
#define GFXCOOPA_ENGINE_PASSES_PRESENT_PASS_H

#include <volk/volk.h>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class PresentPass
 * @brief Samples one already-tonemapped LDR image and writes it to the
 *        swapchain, unmodified.
 *
 * The last pass in a frame. It does no color work of its own -- the source
 * is expected to be display-ready -- so it exists to get an offscreen target
 * onto a swapchain image that a previous pass could not render into directly.
 */
class PresentPass {
public:
    /**
     * @brief Builds the present pipeline and its descriptor set.
     * @param device         Logical device.
     * @param swapchain_pass Render pass for the swapchain image.
     * @param linear_sampler Accepted for signature compatibility and unused;
     *   the sampler that matters is the one passed to set_source_image().
     * @param vert_spv       Fullscreen-triangle vertex shader.
     * @param frag_spv       Fragment shader; samples binding 0 and writes it through.
     */
    PresentPass(coopa::gfx::core::Device& device,
                coopa::gfx::pipeline::RenderPass& swapchain_pass,
                const util::Sampler& linear_sampler,
                const std::string& vert_spv,
                const std::string& frag_spv)
        : stage_(device, swapchain_pass, describe(vert_spv, frag_spv))
    {
        (void)linear_sampler;
    }

    PresentPass(const PresentPass&) = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    /**
     * @brief Points the pass at the image to present.
     * @param image_view     The LDR color image to blit.
     * @param linear_sampler Sampler used to read it.
     */
    void set_source_image(coopa::gfx::TextureView image_view, const util::Sampler& linear_sampler) {
        stage_.set().bind_image(0, image_view, linear_sampler);
    }

    /**
     * @brief Records the fullscreen blit.
     * @param cmd        Command buffer, inside the swapchain render pass.
     * @param viewport_w Swapchain width in pixels.
     * @param viewport_h Swapchain height in pixels.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const {
        stage_.bind(cmd, viewport_w, viewport_h);
        stage_.draw(cmd);
    }

private:
    /// @brief One sampled image at binding 0, no push constants.
    static FullscreenStageDesc describe(const std::string& vert_spv, const std::string& frag_spv) {
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1}}};
        return d;
    }

    FullscreenStage stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_PRESENT_PASS_H
