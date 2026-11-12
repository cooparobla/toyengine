/**
 * @file hiz_pass.h
 * @brief Hierarchical Z-Buffer (Hi-Z) Depth Pyramid Generation Pass.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_HIZ_PASS_H
#define GFXCOOPA_ENGINE_PASSES_HIZ_PASS_H

#include <volk/volk.h>
#include <vma/vk_mem_alloc.h>
#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>
#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class HiZPass
 * @brief Builds a hierarchical depth (Hi-Z) pyramid from the G-buffer depth.
 *
 * The reduction each coarse level applies is whatever `frag_spv` computes:
 * e.g. toyengine's hiz_downsample.frag min() for the conservative pyramid SsrPass
 * marches, or its ao_depth_downsample.frag depth-aware weighted average for the
 * prefiltered pyramid SsaoPass marches.
 *
 * Owns the pyramid image, one view and framebuffer per mip, and one descriptor
 * set per mip. execute() records one fullscreen draw per level.
 */
class HiZPass {
public:
    struct PushConstants {
        glm::ivec2 src_size;
        int is_first_pass;
    };

    /**
     * @param frag_spv       Downsample fragment shader defining the per-level reduction.
     * @param max_mip_levels Cap on the chain length; 0 builds the full chain down to 1x1.
     *                       A consumer whose march never reaches deep mips (SsaoPass caps
     *                       its march at a fixed pixel radius) passes a small cap and skips
     *                       both the memory and the per-frame draws of the unused tail.
     * @param external_level0 Don't render level 0 of the chain. The pyramid image then holds
     *                       levels 1..N only (at half the source resolution), and the
     *                       consumer reads level 0 straight from the source depth it passed
     *                       in. Exact for a chain whose level 0 is a plain copy of that depth
     *                       (both shipped downsample shaders), and saves the full-resolution
     *                       copy pass. Level numbering (max_mip_level(), src_size) stays that
     *                       of the full chain; a consumer sampling mip k of full_hiz_view()
     *                       must subtract one.
     */
    HiZPass(coopa::gfx::core::Device& device,
            coopa::gfx::memory::Allocator& allocator,
            const std::string& vert_spv,
            const std::string& frag_spv,
            uint32_t max_mip_levels = 0,
            bool external_level0 = false)
        : device_(device), allocator_(allocator), max_mip_levels_(max_mip_levels),
          level_base_(external_level0 ? 1u : 0u)
    {
        // 2. Sampler (Nearest filtering for exact depth reads). max_lod is set in recreate()
        // once mip_levels_ is known, so every mip of the Hi-Z pyramid is actually reachable by
        // textureLod() in the SSR raymarch shader (a sampler with maxLod = 0 silently clamps
        // every explicit-LOD read back to mip 0, defeating the whole hierarchy).
        sampler_ = std::make_unique<util::Sampler>(
            device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f
        );

        // 3. Render Pass for R32_SFLOAT attachments
        render_pass_ = std::make_unique<coopa::gfx::pipeline::RenderPass>(
            device,
            VK_FORMAT_R32_SFLOAT,
            VK_FORMAT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );

        // One sampled image at binding 0 (the previous mip), plus the fragment push
        // constants. One set per mip level, reallocated by rebuild_sets() whenever the
        // chain length changes; the pipeline is built once and stays compatible.
        FullscreenStageDesc sd;
        sd.vert_spv = vert_spv;
        sd.frag_spv = frag_spv;
        sd.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                           coopa::gfx::ShaderStage::Fragment, 1}}};
        sd.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        stage_ = std::make_unique<FullscreenStage>(device, *render_pass_, sd);
    }

    ~HiZPass() {
        destroy_resources_();
    }

    HiZPass(const HiZPass&) = delete;
    HiZPass& operator=(const HiZPass&) = delete;

    void recreate(uint32_t width, uint32_t height) {
        destroy_resources_();
        width_  = width;
        height_ = height;
        mip_levels_ = static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1;
        if (max_mip_levels_ > 0) mip_levels_ = std::min(mip_levels_, max_mip_levels_);
        // Levels this pass renders: the full chain, minus level 0 when the consumer reads
        // that one from its own depth (external_level0). A chain of nothing but level 0
        // then owns no image at all -- full_hiz_view() stays null and max_mip_level() is 0.
        rendered_levels_ = mip_levels_ - level_base_;
        if (rendered_levels_ == 0) { stage_->rebuild_sets(0); bound_source_ = coopa::gfx::TextureView{}; return; }
        const uint32_t base_w = std::max(1u, width_  >> level_base_);
        const uint32_t base_h = std::max(1u, height_ >> level_base_);

        // Rebuild the sampler with maxLod = mip_levels_ so textureLod() in ssr.frag can
        // actually reach every coarse mip of the pyramid (see constructor comment above).
        sampler_ = std::make_unique<util::Sampler>(
            device_, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            static_cast<float>(mip_levels_)
        );

        // Allocate image
        VkImageCreateInfo img_info{};
        img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType     = VK_IMAGE_TYPE_2D;
        img_info.format        = VK_FORMAT_R32_SFLOAT;
        img_info.extent        = {base_w, base_h, 1};
        img_info.mipLevels     = rendered_levels_;
        img_info.arrayLayers   = 1;
        img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

        GFX_VK_CHECK(vmaCreateImage(allocator_.handle(), &img_info, &alloc_info, &hiz_image_, &allocation_, nullptr));

        // Create full image view for all mips
        VkImageViewCreateInfo full_view_info{};
        full_view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        full_view_info.image                           = hiz_image_;
        full_view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        full_view_info.format                          = VK_FORMAT_R32_SFLOAT;
        full_view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        full_view_info.subresourceRange.baseMipLevel   = 0;
        full_view_info.subresourceRange.levelCount     = rendered_levels_;
        full_view_info.subresourceRange.baseArrayLayer = 0;
        full_view_info.subresourceRange.layerCount     = 1;

        GFX_VK_CHECK(vkCreateImageView(device_.handle(), &full_view_info, nullptr, &full_view_));

        // Create per-mip views & framebuffers. Image mip m is full-chain level m + level_base_.
        mip_views_.resize(rendered_levels_);
        mip_framebuffers_.resize(rendered_levels_);

        for (uint32_t m = 0; m < rendered_levels_; ++m) {
            uint32_t mw = std::max(1u, base_w >> m);
            uint32_t mh = std::max(1u, base_h >> m);

            VkImageViewCreateInfo view_info{};
            view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image                           = hiz_image_;
            view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format                          = VK_FORMAT_R32_SFLOAT;
            view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.baseMipLevel   = m;
            view_info.subresourceRange.levelCount     = 1;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount     = 1;

            GFX_VK_CHECK(vkCreateImageView(device_.handle(), &view_info, nullptr, &mip_views_[m]));

            VkFramebufferCreateInfo fb_info{};
            fb_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_info.renderPass      = render_pass_->handle();
            fb_info.attachmentCount = 1;
            fb_info.pAttachments    = &mip_views_[m];
            fb_info.width           = mw;
            fb_info.height          = mh;
            fb_info.layers          = 1;

            GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb_info, nullptr, &mip_framebuffers_[m]));
        }

        stage_->rebuild_sets(rendered_levels_);
        bound_source_ = coopa::gfx::TextureView{};   // fresh sets: nothing bound yet
    }

    /// True when execute() with `source` would have to (re)write descriptors -- the first
    /// call after construction/recreate(), or a different source view than last time.
    /// vkUpdateDescriptorSets on a set a pending command buffer references is invalid, so
    /// a caller that overlaps frames in flight must wait for the GPU before such a frame.
    bool needs_descriptor_update(coopa::gfx::TextureView source) const {
        return rendered_levels_ != 0 && source != bound_source_;
    }

    void update_descriptors(coopa::gfx::TextureView gbuffer_depth_view) {
        if (rendered_levels_ == 0) return;
        // Every view this binds is fixed between recreate()s, so an unchanged source means
        // the sets already hold exactly this -- skip the write (see needs_descriptor_update).
        if (gbuffer_depth_view == bound_source_) return;
        bound_source_ = gbuffer_depth_view;
        // The first rendered level samples G-Buffer depth (full-chain level 0 copies it;
        // with external_level0, level 1 reduces it directly).
        stage_->set(0, 0).bind_image(0, gbuffer_depth_view, *sampler_);

        // Rendered level m >= 1 samples mip_views_[m-1] -- mip_views_ itself stays a raw
        // std::vector<VkImageView> (part of the mip-chain machinery with no sealed
        // equivalent; see the class doc), wrapped per-use via detail::wrap() since this
        // is gfxcoopa's own internal code (the leak gate only scopes consumer repos).
        for (uint32_t m = 1; m < rendered_levels_; ++m) {
            stage_->set(0, m).bind_image(0, coopa::gfx::detail::wrap(mip_views_[m - 1]), *sampler_);
        }
    }

    /// @param transition_depth Whether to transition the depth image from
    /// DEPTH_STENCIL_ATTACHMENT_OPTIMAL to SHADER_READ_ONLY_OPTIMAL before reading it.
    /// Pass false when another pyramid pass over the same depth image already ran this
    /// frame and performed the transition (a second one would present a stale oldLayout).
    void execute(coopa::gfx::command::CommandBuffer& cmd,
                 VkImage gbuffer_depth_image,
                 coopa::gfx::TextureView gbuffer_depth_view,
                 bool transition_depth = true)
    {
        update_descriptors(gbuffer_depth_view);

        // 1. Transition G-Buffer depth buffer for shader read
        if (transition_depth) {
            VkImageMemoryBarrier depth_barrier{};
            depth_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            depth_barrier.oldLayout                       = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth_barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            depth_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            depth_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            depth_barrier.image                           = gbuffer_depth_image;
            depth_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
            depth_barrier.subresourceRange.baseMipLevel   = 0;
            depth_barrier.subresourceRange.levelCount     = 1;
            depth_barrier.subresourceRange.baseArrayLayer = 0;
            depth_barrier.subresourceRange.layerCount     = 1;
            depth_barrier.srcAccessMask                   = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depth_barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd.handle(),
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &depth_barrier);
        }

        if (rendered_levels_ == 0) return;
        cmd.bind_pipeline(stage_->pipeline());

        // 2. Loop through the rendered mip levels; L is the full-chain level each one is.
        for (uint32_t m = 0; m < rendered_levels_; ++m) {
            const uint32_t L  = m + level_base_;
            uint32_t mw = std::max(1u, width_ >> L);
            uint32_t mh = std::max(1u, height_ >> L);

            PushConstants pc{};
            pc.src_size      = (L == 0) ? glm::ivec2(width_, height_) : glm::ivec2(std::max(1u, width_ >> (L - 1)), std::max(1u, height_ >> (L - 1)));
            pc.is_first_pass = (L == 0) ? 1 : 0;

            cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);

            VkClearValue clear_val{};
            clear_val.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

            VkRenderPassBeginInfo rp_info{};
            rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp_info.renderPass        = render_pass_->handle();
            rp_info.framebuffer       = mip_framebuffers_[m];
            rp_info.renderArea.offset = {0, 0};
            rp_info.renderArea.extent = {mw, mh};
            rp_info.clearValueCount   = 1;
            rp_info.pClearValues      = &clear_val;

            vkCmdBeginRenderPass(cmd.handle(), &rp_info, VK_SUBPASS_CONTENTS_INLINE);

            cmd.set_viewport(0.0f, 0.0f, static_cast<float>(mw), static_cast<float>(mh));
            cmd.set_scissor(0, 0, mw, mh);

            cmd.bind_descriptor_set(stage_->set(0, m), 0);

            stage_->draw(cmd);

            cmd.end_render_pass();

            // Makes this mip's color write visible to the next mip's (or, on the last
            // iteration, SSR's) fragment-shader read of it. The render pass's EXIT dependency
            // (finalLayout SHADER_READ_ONLY_OPTIMAL; see render_pass.h's needs_exit_dependency)
            // already orders the same write -> read pair, so this barrier is an explicit,
            // per-mip restatement of it. Layout doesn't change (finalLayout already matches
            // the read); this is purely an availability/visibility barrier scoped to the
            // single mip level just written.
            VkImageMemoryBarrier mip_barrier{};
            mip_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            mip_barrier.oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mip_barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mip_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            mip_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            mip_barrier.image                           = hiz_image_;
            mip_barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            mip_barrier.subresourceRange.baseMipLevel   = m;
            mip_barrier.subresourceRange.levelCount     = 1;
            mip_barrier.subresourceRange.baseArrayLayer = 0;
            mip_barrier.subresourceRange.layerCount     = 1;
            mip_barrier.srcAccessMask                   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            mip_barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd.handle(),
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &mip_barrier);
        }
    }

    VkImageView full_hiz_view() const { return full_view_; }
    coopa::gfx::TextureView full_hiz_view_typed() const { return coopa::gfx::detail::wrap(full_view_); }
    /// Deepest level of the FULL chain (level 0 included even when external_level0 leaves
    /// it to the consumer).
    uint32_t max_mip_level() const { return mip_levels_ > 0 ? mip_levels_ - 1 : 0; }
    /// 1 when the consumer reads level 0 from its own depth: mip k of full_hiz_view() is
    /// full-chain level k + this.
    uint32_t level_base() const { return level_base_; }
    const util::Sampler& sampler() const { return *sampler_; }

private:
    coopa::gfx::TextureView bound_source_{};   ///< Source view the sets were last bound to.
    void destroy_resources_() {
        if (full_view_ != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.handle(), full_view_, nullptr);
            full_view_ = VK_NULL_HANDLE;
        }
        for (auto v : mip_views_) {
            if (v != VK_NULL_HANDLE) vkDestroyImageView(device_.handle(), v, nullptr);
        }
        mip_views_.clear();

        for (auto fb : mip_framebuffers_) {
            if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_.handle(), fb, nullptr);
        }
        mip_framebuffers_.clear();

        if (hiz_image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_.handle(), hiz_image_, allocation_);
            hiz_image_ = VK_NULL_HANDLE;
            allocation_ = VK_NULL_HANDLE;
        }
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;

    uint32_t width_          = 0;
    uint32_t height_         = 0;
    uint32_t mip_levels_     = 0; ///< Full-chain level count (level 0 included).
    uint32_t max_mip_levels_ = 0; ///< Constructor cap on the chain length; 0 = full chain.
    uint32_t level_base_     = 0; ///< First level this pass renders (1 with external_level0).
    uint32_t rendered_levels_ = 0; ///< mip_levels_ - level_base_; the image's mip count.

    VkImage       hiz_image_  = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView   full_view_  = VK_NULL_HANDLE;

    std::vector<VkImageView>   mip_views_;
    std::vector<VkFramebuffer> mip_framebuffers_;

    std::unique_ptr<coopa::gfx::pipeline::RenderPass>          render_pass_;
    std::unique_ptr<util::Sampler>   sampler_;
    std::unique_ptr<FullscreenStage> stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_HIZ_PASS_H
