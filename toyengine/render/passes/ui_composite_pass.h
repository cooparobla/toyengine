/**
 * @file ui_composite_pass.h
 * @brief Composites the world-space UI layer over the finished, post-processed frame
 *        (see assets/shaders/ui_composite.frag).
 *
 * This is the stage that keeps UI out of the display-space effects. The world canvases
 * used to be drawn as a guest inside post_target_'s bracket, which put them upstream of
 * the AA pass and TiltShiftPass -- so a moving canvas ghosted under TAA and the whole UI
 * smeared under tilt shift. They now render into their own transparent RGBA8 layer, sized
 * to the letterbox rect, and this pass puts that layer back on top once every display-space
 * effect has already run.
 *
 * Structurally a two-source UpscalePass (toyengine/render/passes/upscale_pass.h): same
 * fullscreen triangle, same nearest sampling, same draw-into-a-LetterboxRect idiom, one
 * extra combined-image-sampler binding. With tilt shift off, the BASE source is still at
 * render_extent_ and this pass performs the nearest-neighbour upscale UpscalePass used to --
 * over an identical rect with identical UV math, so the scene's pixel-art look is unchanged.
 * The UI source is different: it is built at the letterbox rect, so sampling it is an exact
 * texel-for-texel fetch and the UI is never resampled at all.
 */

#ifndef TOYENGINE_RENDER_PASSES_UI_COMPOSITE_PASS_H
#define TOYENGINE_RENDER_PASSES_UI_COMPOSITE_PASS_H

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
 * @class UiCompositePass
 * @brief Draws `base` into a letterboxed viewport with the UI layer composited over it.
 *
 * Both sources are sampled NEAREST and both are UNORM images holding display-referred,
 * sRGB-encoded bytes; the blend is a premultiplied "over" done in that encoded space (see
 * ui_composite.frag). The UI layer must therefore have been drawn with
 * `coopa::gfx::pipeline::BlendMode::AlphaOver` into an alpha-0-cleared target, which is
 * what makes its RGB premultiplied and its alpha a real coverage mask.
 */
class UiCompositePass {
public:
    /**
     * @param device      Vulkan logical device.
     * @param target_pass The render pass this draws into (the overlay target's).
     * @param vert_spv    Fullscreen-triangle vertex shader (fullscreen.vert).
     * @param frag_spv    ui_composite.frag.
     */
    UiCompositePass(coopa::gfx::core::Device& device,
                    coopa::gfx::pipeline::RenderPass& target_pass,
                    const std::string& vert_spv,
                    const std::string& frag_spv)
        : device_(device)
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

    UiCompositePass(const UiCompositePass&) = delete;
    UiCompositePass& operator=(const UiCompositePass&) = delete;

    /**
     * @brief Binds the post-processed scene image (tilt shift's result, or the AA/post
     *        target when tilt shift is off).
     *
     * Must be called before Renderer::begin_frame() -- DescriptorSet::bind_image calls
     * vkUpdateDescriptorSets immediately, which is unsafe mid-frame.
     */
    void set_base_image(coopa::gfx::TextureView view,
                        const coopa::gfx::engine::util::Sampler& nearest_sampler) {
        desc_set_->bind_image(0, view, nearest_sampler);
    }

    /**
     * @brief Binds the world-UI layer, which the caller must have built at exactly the rect
     *        draw() will be given -- that is what makes the fetch 1:1. Same mid-frame caveat as
     *        set_base_image().
     */
    void set_ui_image(coopa::gfx::TextureView view,
                      const coopa::gfx::engine::util::Sampler& nearest_sampler) {
        desc_set_->bind_image(1, view, nearest_sampler);
    }

    /**
     * @brief Draws into the centred destination rect; the target's own clear fills the bars.
     *
     * The viewport height is POSITIVE, and deliberately so: fullscreen.vert maps uv.y == 0
     * to NDC -1, while OffscreenTarget::begin() leaves a NEGATIVE-height viewport behind
     * (it flips Y for the 3D passes). Inheriting that one would present the whole frame
     * upside down -- the same hazard UiWorldPass::draw() documents from the other side.
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

#endif // TOYENGINE_RENDER_PASSES_UI_COMPOSITE_PASS_H
