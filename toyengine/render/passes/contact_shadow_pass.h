/**
 * @file contact_shadow_pass.h
 * @brief Screen-space contact shadows as their own pass, with a converging temporal resolve.
 */

#ifndef TOYENGINE_RENDER_PASSES_CONTACT_SHADOW_PASS_H
#define TOYENGINE_RENDER_PASSES_CONTACT_SHADOW_PASS_H

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/ssr_pass.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class ContactShadowPass
 * @brief Marches the screen-space contact shadow into its own buffer, then averages that buffer
 *        over frames the way the reflection and AO chains average theirs.
 *
 * Unity HDRP computes contact shadows into a dedicated texture (`_ContactShadowTexture`) rather
 * than inline in its lighting shader, and this follows that shape for the same reason plus one
 * more: inline, the term had no buffer of its own, so its only temporal filter was the
 * whole-frame TAA — whose variance clip rejects history precisely on thin, high-contrast
 * features under motion, which is exactly what a contact shadow is.
 *
 * Three stages, mirroring SsaoPass's raw → resolve shape:
 *  - **March** (`contact_shadow.frag`) writes raw occlusion: one ray toward the sun per pixel,
 *    one scene-depth fetch per step (`contact_shadow_body.glsl`, Unreal's screen-space contact
 *    shadow shape).
 *  - **Resolve** (`contact_shadow_resolve.frag`) is the scalar counterpart of gfxcoopa's
 *    `ssr_resolve.frag`: same push constants and bindings, same converging `1/N` schedule against
 *    the shared per-pixel count (TemporalHistoryPass), minus the vec4/YCoCg machinery.
 *  - **History copy**: one resolve target plus one history image, copied at the end of every
 *    execute(). SsrPass and TemporalHistoryPass ping-pong instead (one consumer set per
 *    parity, selected per frame); this pass's lone consumer, the lighting draw, binds its
 *    output once, and the copy is a small full-res R8-class transfer, so it keeps the copy.
 *
 * The consumer (`pixel_lighting.frag`, and `debug_view.frag`'s contact_shadows channel) samples
 * `output_view_typed()` and applies strength and per-light darkness itself, exactly as it did
 * with the inline call: this pass emits the RAW geometric occlusion.
 */
class ContactShadowPass {
public:
    /// Format of the march and resolve targets: one occlusion scalar per pixel. The dedicated
    /// scalar resolve (contact_shadow_resolve.frag) is what lets this be R16F rather than the
    /// RGBA16F the shared vec4 SSR resolve required -- a quarter of the bandwidth for the march
    /// write, the resolve and the history copy alike.
    static constexpr coopa::gfx::Format kFormat = coopa::gfx::Format::R16_Sfloat;

    /// Per-frame parameters. Mirrors the temporal half of SsrPass::Params, since the resolve is
    /// literally the same shader.
    struct Params {
        /// Current clip space -> previous frame's clip space, composed in double precision by the
        /// caller. See SsrPass::Params::reproject for why the composition order matters.
        glm::mat4 reproject       = glm::mat4(1.0f);
        bool      reproject_valid = false;
        /// Accumulation depth (contact_shadow_temporal_frames). 0 makes the resolve a passthrough.
        int       temporal_frames = 16;
        /// Variance-clipping width, in standard deviations of the 3x3 neighbourhood.
        float     temporal_gamma  = 2.0f;
        /// True once the camera has been still long enough for the average to top up; accepted
        /// history is then held verbatim, which is what keeps a resting image byte-static.
        bool      frozen          = false;
        /// Which of the two count images update_descriptors() bound holds THIS frame's counts
        /// (TemporalHistoryPass::current_parity()).
        uint32_t  count_parity    = 0;
    };

    /**
     * @param device        Vulkan logical device.
     * @param allocator     VMA allocator for the three targets.
     * @param camera_layout The camera UBO set layout (set 0), owned by the caller.
     * @param light_layout  The light UBO set layout (set 1), owned by the caller.
     * @param width         Render width in pixels.
     * @param height        Render height in pixels.
     * @param vert_spv      Fullscreen-triangle vertex shader (fullscreen.vert).
     * @param march_spv     contact_shadow.frag.spv.
     * @param resolve_spv   contact_shadow_resolve.frag.spv (SsrPass::ResolvePushConstants layout).
     */
    ContactShadowPass(coopa::gfx::core::Device& device,
                      coopa::gfx::memory::Allocator& allocator,
                      const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                      const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                      uint32_t width, uint32_t height,
                      const std::string& vert_spv,
                      const std::string& march_spv,
                      const std::string& resolve_spv)
        : device_(device), width_(width), height_(height)
    {
        using namespace coopa::gfx;

        // NEAREST throughout: this is an occlusion mask sampled at texel centres, and the
        // G-buffer taps the march makes are discrete per-pixel data that must never be blended
        // across a silhouette. The resolve's history read is the one exception — it samples at a
        // reprojected, non-texel-aligned UV.
        SamplerDesc nearest_clamp;
        nearest_clamp.min = nearest_clamp.mag = Filter::Nearest;
        nearest_clamp.mipmap  = MipmapMode::Nearest;
        nearest_clamp.address = AddressMode::ClampToEdge;
        nearest_sampler_ = std::make_unique<engine::util::Sampler>(device, nearest_clamp);

        SamplerDesc linear_clamp = nearest_clamp;
        linear_clamp.min = linear_clamp.mag = Filter::Linear;
        linear_sampler_  = std::make_unique<engine::util::Sampler>(device, linear_clamp);

        march_target_ = std::make_unique<engine::targets::OffscreenTarget>(
            device, allocator, width, height, kFormat, engine::targets::kColorOnly);
        resolved_target_ = std::make_unique<engine::targets::OffscreenTarget>(
            device, allocator, width, height, kFormat, engine::targets::kColorOnly);
        history_image_ = std::make_unique<memory::Image>(
            device, allocator, width, height, VK_FORMAT_R16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO);

        vert_shader_    = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
        march_shader_   = std::make_unique<pipeline::Shader>(device, march_spv, ShaderStage::Fragment);
        resolve_shader_ = std::make_unique<pipeline::Shader>(device, resolve_spv, ShaderStage::Fragment);

        // Set 2 of the march: the three-binding G-buffer shape every other consumer uses, plus
        // the scene depth buffer the march compares its ray against (binding 3).
        gbuf_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, ShaderStage::Fragment)
                .combined_sampler(1, ShaderStage::Fragment)
                .combined_sampler(2, ShaderStage::Fragment)
                .combined_sampler(3, ShaderStage::Fragment)
                .build(device));
        // Set 0 of the resolve: current, history, depth, shared count (ssr_resolve.frag's shape).
        resolve_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, ShaderStage::Fragment)
                .combined_sampler(1, ShaderStage::Fragment)
                .combined_sampler(2, ShaderStage::Fragment)
                .combined_sampler(3, ShaderStage::Fragment)
                .build(device));

        pipeline::DescriptorPoolBuilder pool_builder;
        // Two resolve sets: one per parity of TemporalHistoryPass's ping-pong count buffer,
        // so the right one is selected per frame instead of a descriptor being rewritten.
        pool_builder.add_sets(*gbuf_layout_, 1).add_sets(*resolve_layout_, 2);
        desc_pool_ = std::make_unique<pipeline::DescriptorPool>(pool_builder.build(device));

        gbuf_set_ = std::make_unique<pipeline::DescriptorSet>(device, *desc_pool_, *gbuf_layout_);
        for (uint32_t c = 0; c < 2; ++c) {
            resolve_sets_[c] = std::make_unique<pipeline::DescriptorSet>(device, *desc_pool_, *resolve_layout_);
        }

        pipeline::PipelineDesc common;
        common.vertex      = VertexLayout::none();
        common.raster.cull = CullMode::None;
        common.depth.test  = false;
        common.depth.write = false;

        pipeline::PipelineDesc march_desc = common;
        march_desc.shaders            = {vert_shader_.get(), march_shader_.get()};
        march_desc.descriptor_layouts = {&camera_layout, &light_layout, gbuf_layout_.get()};
        march_pipeline_ = std::make_unique<pipeline::Pipeline>(
            device, march_target_->render_pass_object(), march_desc);

        pipeline::PipelineDesc resolve_desc = common;
        resolve_desc.shaders            = {vert_shader_.get(), resolve_shader_.get()};
        resolve_desc.descriptor_layouts = {resolve_layout_.get()};
        resolve_desc.push_constants     = {
            {ShaderStage::Fragment, 0,
             sizeof(coopa::gfx::engine::passes::SsrPass::ResolvePushConstants)}};
        resolve_pipeline_ = std::make_unique<pipeline::Pipeline>(
            device, resolved_target_->render_pass_object(), resolve_desc);
    }

    /// Binds the live G-buffer, the scene depth the resolve reprojects from, and both parities
    /// of the shared accumulation count (TemporalHistoryPass::count_view_typed(0/1); execute()
    /// selects with Params::count_parity). Called once at construction (and after any resize):
    /// rebinding per frame is unsafe under an overlapped-frame pipeline.
    void update_descriptors(coopa::gfx::TextureView g0, coopa::gfx::TextureView g1,
                            coopa::gfx::TextureView g2,
                            coopa::gfx::TextureView depth,
                            coopa::gfx::TextureView count0, coopa::gfx::TextureView count1,
                            const coopa::gfx::engine::util::Sampler& count_sampler)
    {
        // NEAREST. The march fetches the G-buffer at arbitrary UVs along its ray, where bilinear
        // filtering blends world positions across silhouette edges into points that exist on no
        // real surface -- the same rule SsrPass keeps a dedicated nearest sampler for. The body
        // uses texelFetch for those reads so the sampler is moot there, but the binding should
        // not imply that blending this data is ever acceptable.
        gbuf_set_->bind_image(0, g0, *nearest_sampler_);
        gbuf_set_->bind_image(1, g1, *nearest_sampler_);
        gbuf_set_->bind_image(2, g2, *nearest_sampler_);
        // The march's per-step occluder test: one depth texel per step (D32_SFLOAT, NEAREST).
        gbuf_set_->bind_image(3, depth, *nearest_sampler_);

        for (uint32_t c = 0; c < 2; ++c) {
            resolve_sets_[c]->bind_image(0, march_target_->color_view_typed(), *nearest_sampler_);
            // LINEAR: the history is read at a reprojected, non-texel-aligned UV.
            resolve_sets_[c]->bind_image(1, history_image_->view_typed(), *linear_sampler_);
            // NEAREST is mandatory: D32_SFLOAT is not guaranteed to support linear filtering.
            resolve_sets_[c]->bind_image(2, depth, *nearest_sampler_);
            resolve_sets_[c]->bind_image(3, c == 0 ? count0 : count1, count_sampler);
        }
    }

    void execute(coopa::gfx::command::CommandBuffer& cmd,
                 const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const coopa::gfx::pipeline::DescriptorSet& light_set,
                 const Params& params)
    {
        // On the first frame the history image has never been written and sits in UNDEFINED;
        // transition it before it is bound as a sampled image. The resolve does not read it in
        // that case (history_valid = 0), but the descriptor still needs a valid layout at draw
        // time regardless of the runtime branch -- the same one-time transition SsrPass and
        // SsaoPass both perform.
        if (!history_initialized_) {
            VkImageMemoryBarrier barrier{};
            barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.image                           = history_image_->handle();
            barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount     = 1;
            barrier.subresourceRange.layerCount     = 1;
            barrier.srcAccessMask                   = 0;
            barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // --- 1. March ---
        march_target_->begin(cmd);
        cmd.bind_pipeline(*march_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);
        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(light_set, 1);
        cmd.bind_descriptor_set(*gbuf_set_, 2);
        cmd.draw(3);
        march_target_->end(cmd);

        // --- 2. Temporal resolve ---
        resolved_target_->begin(cmd);
        cmd.bind_pipeline(*resolve_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);

        coopa::gfx::engine::passes::SsrPass::ResolvePushConstants rpc{};
        rpc.reproject     = params.reproject;
        rpc.resolution_x  = static_cast<float>(width_);
        rpc.resolution_y  = static_cast<float>(height_);
        rpc.max_accum     = static_cast<float>(params.temporal_frames);
        // Unreachable: this pass always binds a real count buffer. Carried only because the
        // shared push block declares it.
        rpc.blend_factor  = 0.0f;
        rpc.history_valid = (history_initialized_ && params.reproject_valid) ? 1 : 0;
        rpc.gamma         = params.temporal_gamma;
        rpc.frozen        = params.frozen ? 1 : 0;
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, rpc);
        cmd.bind_descriptor_set(*resolve_sets_[params.count_parity & 1], 0);
        cmd.draw(3);
        resolved_target_->end(cmd);

        copy_history_(cmd);
        history_initialized_ = true;
    }

    /// Clears the output to 0 (no occlusion) without marching -- for a caller that skips
    /// execute() while the feature is off. Leaves the image in SHADER_READ_ONLY_OPTIMAL, the
    /// layout its consumers' descriptors expect, and holding exactly what a disabled march
    /// would resolve to. Also drops the history, which no longer describes the output.
    void clear_output(coopa::gfx::command::CommandBuffer& cmd) {
        resolved_target_->begin(cmd, {{0.0f, 0.0f, 0.0f, 0.0f}});
        resolved_target_->end(cmd);
        history_initialized_ = false;
    }

    /// @brief The resolved occlusion buffer. One image for the pass's lifetime, so a consumer
    /// binds it once at setup and always reads this frame's result.
    coopa::gfx::TextureView output_view_typed() const {
        return resolved_target_->color_view_typed();
    }

    /// @brief NEAREST/ClampToEdge sampler consumers should read the output through.
    const coopa::gfx::engine::util::Sampler& sampler() const { return *nearest_sampler_; }

    /// Drops the accumulated history. Call on any frame execute() is skipped, so the next
    /// enabled frame does not average against occlusion captured under an arbitrary old pose.
    void invalidate_history() { history_initialized_ = false; }

private:
    /// Copies the resolve into the history image for next frame. Structurally identical to
    /// SsrPass::copy_history_(), over this pass's single image pair.
    void copy_history_(coopa::gfx::command::CommandBuffer& cmd) {
        VkImage src = resolved_target_->color_image_object()->handle();
        VkImage dst = history_image_->handle();

        VkImageMemoryBarrier barriers[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            barriers[i].sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[i].subresourceRange.levelCount = 1;
            barriers[i].subresourceRange.layerCount = 1;
        }
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image         = src;
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image         = dst;
        barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy copy{};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent                    = {width_, height_, 1};
        vkCmdCopyImage(cmd.handle(), src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    }

    coopa::gfx::core::Device& device_;
    uint32_t width_;
    uint32_t height_;

    std::unique_ptr<coopa::gfx::engine::util::Sampler> nearest_sampler_;
    std::unique_ptr<coopa::gfx::engine::util::Sampler> linear_sampler_;

    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> march_target_;
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> resolved_target_;
    /// Last frame's copy of the resolve, the surface the reprojection samples.
    std::unique_ptr<coopa::gfx::memory::Image> history_image_;
    bool history_initialized_ = false;

    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> march_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> resolve_shader_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> gbuf_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> resolve_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       gbuf_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       resolve_sets_[2];   ///< [count parity]

    std::unique_ptr<coopa::gfx::pipeline::Pipeline> march_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> resolve_pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_CONTACT_SHADOW_PASS_H
