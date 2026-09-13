/**
 * @file fullscreen_blit_pass.h
 * @brief Draws a single sampled texture over the whole target, unlit and depth-test-free.
 *
 * The minimal fullscreen pass: one combined-image-sampler binding, a fullscreen
 * triangle, no camera/light/shadow sets. Used for diagnostic views that replace the
 * lit image outright -- see `PixelRenderPipeline`'s `ssao_debug_pass_`, which binds the
 * exact occlusion image lighting itself consumes so the debug view can't disagree with
 * it. The fragment shader is supplied by the caller, so the same pass backs any
 * "show me this one texture" view.
 */

#ifndef TOYENGINE_RENDER_PASSES_FULLSCREEN_BLIT_PASS_H
#define TOYENGINE_RENDER_PASSES_FULLSCREEN_BLIT_PASS_H

#include <memory>
#include <string>

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

#endif // TOYENGINE_RENDER_PASSES_FULLSCREEN_BLIT_PASS_H
