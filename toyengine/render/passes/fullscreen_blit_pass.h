/**
 * @file fullscreen_blit_pass.h
 * @brief Draws a single sampled texture over the whole target, unlit and depth-test-free.
 *
 * The minimal fullscreen pass: one combined-image-sampler binding, a fullscreen
 * triangle, no camera/light/shadow sets -- for a diagnostic view that needs nothing
 * beyond a single already-computed texture to show. The fragment shader is supplied by
 * the caller, so the same pass can back any "show me this one texture" view; toyengine's
 * own `debug_view` channels currently need the camera/light/shadow sets this pass
 * doesn't declare (see `ToyRenderPipeline::debug_view_pass_`, a gfxcoopa
 * `DeferredLightingPass` instance instead), so nothing in this repo instantiates it yet.
 */

#ifndef TOYENGINE_RENDER_PASSES_FULLSCREEN_BLIT_PASS_H
#define TOYENGINE_RENDER_PASSES_FULLSCREEN_BLIT_PASS_H

#include <memory>
#include <string>

#include <gfxcoopa/command/command_buffer.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class FullscreenBlitPass
 * @brief Samples one texture across the full viewport, with no lighting and no depth test.
 */
class FullscreenBlitPass {
public:
    /**
     * @param device      Vulkan logical device.
     * @param target_pass The render pass this draws into.
     * @param vert_spv    Fullscreen-triangle vertex shader (fullscreen.vert).
     * @param frag_spv    Fragment shader deciding how the bound texture is displayed.
     */
    FullscreenBlitPass(coopa::gfx::core::Device& device,
                       coopa::gfx::pipeline::RenderPass& target_pass,
                       const std::string& vert_spv,
                       const std::string& frag_spv);

    FullscreenBlitPass(const FullscreenBlitPass&) = delete;
    FullscreenBlitPass& operator=(const FullscreenBlitPass&) = delete;

    /**
     * @brief Binds the texture to display.
     *
     * Must be called before Renderer::begin_frame() -- DescriptorSet::bind_image()
     * issues vkUpdateDescriptorSets immediately, which is unsafe mid-frame.
     */
    void set_source_image(coopa::gfx::TextureView view, const coopa::gfx::engine::util::Sampler& sampler) {
        desc_set_->bind_image(0, view, sampler);
    }

    /** @brief Draws the fullscreen triangle over a viewport of the given size. */
    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const;

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

#endif // TOYENGINE_RENDER_PASSES_FULLSCREEN_BLIT_PASS_H
