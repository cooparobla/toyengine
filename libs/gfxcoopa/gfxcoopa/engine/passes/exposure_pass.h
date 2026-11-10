/**
 * @file exposure_pass.h
 * @brief Auto-exposure (eye adaptation) metering into a 1x1 target.
 *
 * Meters the geometric-mean luminance of an HDR image and adapts an exposure
 * multiplier toward it over time -- see the caller's exposure.frag (toyengine's
 * assets/shaders/exposure.frag) for the metering and adaptation model. The
 * result is a 1x1 R16F texture a tonemap consumer multiplies its own fixed
 * exposure by (StylizePass binds it at set 0 binding 5).
 *
 * One target plus a history image it is copied into after each draw -- the same
 * shape SsrPass uses, and NOT a two-target ping-pong. The distinction is not
 * about cost (at 1x1 neither is measurable); it is that a ping-pong can only
 * expose ONE of its two slots to a consumer whose descriptor is bound once, and
 * that slot is written every OTHER frame. The exposure the tonemap sees would
 * then step every second frame instead of every frame, which reads as a visible
 * 2-frame stutter while the eye is adapting -- and is exactly what made
 * `image_settles_after_camera_stops` fail with a period-2 decay curve. Here
 * `target_` is written every frame and is what the consumer samples.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_EXPOSURE_PASS_H
#define GFXCOOPA_ENGINE_PASSES_EXPOSURE_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/types/texture_view.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class ExposurePass
 * @brief Computes an adapting exposure multiplier from an HDR image.
 */
class ExposurePass {
public:
    /// @brief Matches exposure.frag's ExposurePush block.
    struct PushConstants {
        float dt            = 0.016f; ///< Seconds since the last frame.
        float speed_up      = 3.0f;   ///< Adaptation rate (1/sec) when the scene gets brighter.
        float speed_down    = 1.0f;   ///< Adaptation rate (1/sec) when the scene gets darker.
        float compensation  = 1.0f;   ///< Multiplier on the metered exposure.
        float min_exposure  = 0.05f;  ///< Clamps on the result, as exposure multipliers.
        float max_exposure  = 8.0f;
        int   history_valid = 0;      ///< 0 adopts the metered target outright (first frame).
    };

    /**
     * @param vert_spv Fullscreen-triangle vertex shader (the shared fullscreen.vert).
     * @param frag_spv exposure.frag.
     */
    ExposurePass(coopa::gfx::core::Device& device,
                 coopa::gfx::memory::Allocator& allocator,
                 const std::string& vert_spv,
                 const std::string& frag_spv)
        : linear_sampler_(coopa::gfx::engine::util::Sampler::linear(device))
    {
        target_ = std::make_unique<targets::OffscreenTarget>(
            device, allocator, 1, 1, coopa::gfx::Format::R16_Sfloat, targets::kColorOnly);
        // Last frame's result, which the shader blends against. TRANSFER_DST so
        // copy_history_() can vkCmdCopyImage target_ into it after each draw.
        history_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device, allocator, 1, 1, VK_FORMAT_R16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO);
        stage_ = std::make_unique<FullscreenStage>(
            device, target_->render_pass_object(), describe(vert_spv, frag_spv));
    }

    ExposurePass(const ExposurePass&) = delete;
    ExposurePass& operator=(const ExposurePass&) = delete;

    /**
     * @brief Binds the HDR image to meter. Call ONCE, at construction time
     *        (DescriptorSet::bind_image updates descriptors immediately).
     */
    void set_source_image(coopa::gfx::TextureView hdr) {
        stage_->set(0).bind_image(0, hdr, linear_sampler_);
        stage_->set(0).bind_image(1, history_image_->view_typed(), linear_sampler_);
    }

    /**
     * @brief Records one metering draw, then copies the result into the history.
     *
     * Opens and closes its own render pass (unlike the fullscreen passes that
     * draw as guests into a caller's bracket) -- it writes its own 1x1 target,
     * which nothing else shares.
     */
    void execute(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& params) {
        // history_image_ is sampled by the very first draw but only written by the copy at
        // the end of one, so on frame 0 it is still in VK_IMAGE_LAYOUT_UNDEFINED while its
        // descriptor claims SHADER_READ_ONLY_OPTIMAL. A dynamic branch skipping the read
        // (history_valid == 0) does NOT excuse this: the layout must match at draw time
        // regardless of what the shader does with the value.
        if (!initialized_) {
            VkImageMemoryBarrier barrier{};
            barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.image                           = history_image_->handle();
            barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel   = 0;
            barrier.subresourceRange.levelCount     = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = 1;
            barrier.srcAccessMask                   = 0;
            barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        PushConstants pc = params;
        pc.history_valid = initialized_ ? 1 : 0;

        target_->begin(cmd);
        stage_->draw(cmd, 1, 1, coopa::gfx::ShaderStage::Fragment, pc);
        target_->end(cmd);

        copy_history_(cmd);
        initialized_ = true;
    }

    /**
     * @brief The 1x1 exposure texture a consumer samples.
     *
     * `target_`, which every frame's draw writes -- so a consumer binding this
     * once sees a value that is current, not one updated on alternate frames.
     */
    coopa::gfx::TextureView output_view_typed() const { return target_->color_view_typed(); }
    const coopa::gfx::engine::util::Sampler& sampler() const { return linear_sampler_; }

    /// @brief Drops the adaptation history, so the next execute() adopts its metered target
    /// outright instead of ramping from a value measured against a different image.
    void invalidate_history() { initialized_ = false; }

private:
    /// @brief Set 0: the HDR image to meter, plus last frame's 1x1 result.
    static FullscreenStageDesc describe(const std::string& vert_spv, const std::string& frag_spv) {
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
        };
        d.push_constants = {{ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        return d;
    }

    /// @brief Copies target_ into history_image_, so the next frame's draw has
    /// something to blend against. Same shape as SsrPass::copy_history_.
    void copy_history_(coopa::gfx::command::CommandBuffer& cmd) {
        VkImageMemoryBarrier barriers[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            barriers[i].sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[i].srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[i].subresourceRange.baseMipLevel   = 0;
            barriers[i].subresourceRange.levelCount     = 1;
            barriers[i].subresourceRange.baseArrayLayer = 0;
            barriers[i].subresourceRange.layerCount     = 1;
            barriers[i].srcAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
        }
        barriers[0].image        = target_->color_image_object()->handle();
        barriers[0].newLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image        = history_image_->handle();
        barriers[1].newLayout    = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = {1, 1, 1};
        vkCmdCopyImage(cmd.handle(), target_->color_image_object()->handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, history_image_->handle(),
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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

    coopa::gfx::engine::util::Sampler          linear_sampler_;
    std::unique_ptr<targets::OffscreenTarget>  target_;
    std::unique_ptr<coopa::gfx::memory::Image> history_image_;
    std::unique_ptr<FullscreenStage>           stage_;
    bool initialized_ = false;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_EXPOSURE_PASS_H
