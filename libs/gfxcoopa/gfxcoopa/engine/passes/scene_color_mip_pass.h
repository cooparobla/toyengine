/**
 * @file scene_color_mip_pass.h
 * @brief Prefiltered mip chain of the deferred-lit HDR scene colour, for SSR cone tracing.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SCENE_COLOR_MIP_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SCENE_COLOR_MIP_PASS_H

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

/// Same per-mip render infrastructure as HiZPass (bypasses memory::Image, which hardcodes
/// mipLevels = 1, and calls vmaCreateImage directly), but for the deferred-lit HDR scene colour
/// instead of depth: RGBA16F, linear filtering + linear mip blending, 2x2 box-average
/// downsampling instead of a conservative min reduction. Consumed by ssr.frag's cone trace,
/// which picks a roughness/distance-driven LOD to approximate a blurred glossy reflection.
class SceneColorMipPass {
public:
    struct PushConstants {
        glm::ivec2 src_size;
        int is_first_pass;
    };

    // Cap the chain. A full pyramid at 1920x1080 is 11 mips, and mip 8+ is a screen-wide
    // average -- a "reflection" of nothing in particular, at the cost of a render pass each.
    // 7 mips reaches a 128-texel cone footprint, well past anything a plausible roughness
    // produces at plausible ray lengths.
    static constexpr uint32_t kMaxMips = 7;

    SceneColorMipPass(coopa::gfx::core::Device& device,
                       coopa::gfx::memory::Allocator& allocator,
                       const std::string& vert_spv,
                       const std::string& frag_spv)
        : device_(device), allocator_(allocator)
    {
        // 1. Shaders. vert_spv is the caller's shared fullscreen-triangle vertex shader
        // (toyengine's fullscreen.vert), the same one every other fullscreen pass uses.
        // 2. Sampler. max_lod and mipmap_mode are set in recreate() once mip_levels_ is known --
        // a sampler with maxLod = 0 silently clamps every explicit-LOD read back to mip 0 (the
        // exact trap documented in hiz_pass.h), and MIPMAP_MODE_NEAREST would make the cone LOD
        // quantise to integer mips, showing as visible banding rings on a curved glossy surface.
        sampler_ = std::make_unique<util::Sampler>(
            device, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, VK_SAMPLER_MIPMAP_MODE_LINEAR
        );

        // 3. Render Pass for RGBA16F attachments, no depth (same "no depth" form as HiZPass).
        render_pass_ = std::make_unique<coopa::gfx::pipeline::RenderPass>(
            device,
            VK_FORMAT_R16G16B16A16_SFLOAT,
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

    ~SceneColorMipPass() {
        destroy_resources_();
    }

    SceneColorMipPass(const SceneColorMipPass&) = delete;
    SceneColorMipPass& operator=(const SceneColorMipPass&) = delete;

    void recreate(uint32_t width, uint32_t height) {
        destroy_resources_();
        width_  = width;
        height_ = height;
        mip_levels_ = std::min(kMaxMips,
            static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1);

        // Rebuild the sampler with maxLod = mip_levels_ AND mipmapMode = LINEAR. Both matter --
        // see the constructor comment.
        sampler_ = std::make_unique<util::Sampler>(
            device_, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            static_cast<float>(mip_levels_), VK_SAMPLER_MIPMAP_MODE_LINEAR
        );

        // Allocate image
        VkImageCreateInfo img_info{};
        img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType     = VK_IMAGE_TYPE_2D;
        img_info.format        = VK_FORMAT_R16G16B16A16_SFLOAT;
        img_info.extent        = {width_, height_, 1};
        img_info.mipLevels     = mip_levels_;
        img_info.arrayLayers   = 1;
        img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST: copy_level0_from() writes mip 0 directly (the previous frame's final HDR).
        img_info.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = VMA_MEMORY_USAGE_AUTO;

        GFX_VK_CHECK(vmaCreateImage(allocator_.handle(), &img_info, &alloc_info, &color_image_, &allocation_, nullptr));

        // Create full image view for all mips
        VkImageViewCreateInfo full_view_info{};
        full_view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        full_view_info.image                           = color_image_;
        full_view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        full_view_info.format                          = VK_FORMAT_R16G16B16A16_SFLOAT;
        full_view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        full_view_info.subresourceRange.baseMipLevel   = 0;
        full_view_info.subresourceRange.levelCount     = mip_levels_;
        full_view_info.subresourceRange.baseArrayLayer = 0;
        full_view_info.subresourceRange.layerCount     = 1;

        GFX_VK_CHECK(vkCreateImageView(device_.handle(), &full_view_info, nullptr, &full_view_));

        // Create per-mip views & framebuffers
        mip_views_.resize(mip_levels_);
        mip_framebuffers_.resize(mip_levels_);

        for (uint32_t m = 0; m < mip_levels_; ++m) {
            uint32_t mw = std::max(1u, width_ >> m);
            uint32_t mh = std::max(1u, height_ >> m);

            VkImageViewCreateInfo view_info{};
            view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image                           = color_image_;
            view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format                          = VK_FORMAT_R16G16B16A16_SFLOAT;
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

        stage_->rebuild_sets(mip_levels_);
        bound_source_ = coopa::gfx::TextureView{};   // fresh sets: nothing bound yet
    }

    /// True when execute() with `source` would have to (re)write descriptors -- the first
    /// call after construction/recreate(), or a different source view than last time.
    /// vkUpdateDescriptorSets on a set a pending command buffer references is invalid, so
    /// a caller that overlaps frames in flight must wait for the GPU before such a frame.
    bool needs_descriptor_update(coopa::gfx::TextureView source) const {
        return mip_levels_ != 0 && source != bound_source_;
    }

    void update_descriptors(coopa::gfx::TextureView scene_color_view) {
        if (mip_levels_ == 0) return;
        // Every view this binds is fixed between recreate()s, so an unchanged source means
        // the sets already hold exactly this -- skip the write (see needs_descriptor_update).
        if (scene_color_view == bound_source_) return;
        bound_source_ = scene_color_view;
        // Level 0 samples the deferred-lit HDR scene colour (post skybox).
        stage_->set(0, 0).bind_image(0, scene_color_view, *sampler_);

        // Level m >= 1 samples mip_views_[m-1] -- mip_views_ itself stays a raw
        // std::vector<VkImageView> (part of the mip-chain machinery with no sealed
        // equivalent), wrapped per-use via detail::wrap() since this is gfxcoopa's own
        // internal code (the leak gate only scopes consumer repos).
        for (uint32_t m = 1; m < mip_levels_; ++m) {
            stage_->set(0, m).bind_image(0, coopa::gfx::detail::wrap(mip_views_[m - 1]), *sampler_);
        }
    }

    /**
     * @brief Builds the chain. `skip_level0` keeps mip 0 as it is -- what copy_level0_from()
     *        put there at the end of the previous frame -- and only rebuilds mips 1+ from it.
     *        `scene_color_view` stays bound as level 0's source either way (and is drawn on
     *        frames with no copied level 0), so switching between the two never rewrites a
     *        descriptor a frame in flight still references.
     */
    void execute(coopa::gfx::command::CommandBuffer& cmd, coopa::gfx::TextureView scene_color_view,
                 bool skip_level0 = false) {
        update_descriptors(scene_color_view);

        // No pre-loop barrier for the source: the offscreen render pass's finalLayout is
        // already SHADER_READ_ONLY_OPTIMAL (matching the existing convention -- the SSR pass
        // already reads offscreen_target_->color_view() right after end() with no barrier).

        cmd.bind_pipeline(stage_->pipeline());

        for (uint32_t m = skip_level0 ? 1u : 0u; m < mip_levels_; ++m) {
            uint32_t mw = std::max(1u, width_ >> m);
            uint32_t mh = std::max(1u, height_ >> m);

            PushConstants pc{};
            pc.src_size      = (m == 0) ? glm::ivec2(width_, height_) : glm::ivec2(std::max(1u, width_ >> (m - 1)), std::max(1u, height_ >> (m - 1)));
            pc.is_first_pass = (m == 0) ? 1 : 0;

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

            // Per-mip availability/visibility barrier making this mip's colour write visible
            // to the next mip's (or SSR's) fragment-shader read of it -- an explicit restatement
            // of the render pass's exit dependency (see hiz_pass.h's identical barrier).
            VkImageMemoryBarrier mip_barrier{};
            mip_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            mip_barrier.oldLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mip_barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            mip_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            mip_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            mip_barrier.image                           = color_image_;
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

    /**
     * @brief Copies `src` (same size, RGBA16F, in SHADER_READ_ONLY_OPTIMAL) into mip 0, leaving
     *        both images shader-readable again. Call at the END of a frame with the frame's final
     *        HDR image; the next frame's execute(..., skip_level0 = true) then mips it, which is
     *        how SSR samples the previous frame's final colour (Unreal's PrevSceneColor).
     *
     * One image is enough even with frames in flight: every reader of mip 0 in a frame records
     * before this copy, the pre-barrier orders the copy after them, and the post-barrier orders
     * the next frame's reads after the copy (barriers act across submissions on one queue).
     * Mip 0 must already be shader-readable (execute() has run at least once since recreate()).
     */
    void copy_level0_from(coopa::gfx::command::CommandBuffer& cmd, VkImage src) {
        if (mip_levels_ == 0) return;
        auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                           VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout           = from;
            b.newLayout           = to;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = img;
            b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask       = sa;
            b.dstAccessMask       = da;
            vkCmdPipelineBarrier(cmd.handle(), ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        const VkPipelineStageFlags producers = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier(src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                producers, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(color_image_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                producers, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {width_, height_, 1};
        vkCmdCopyImage(cmd.handle(), src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       color_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        barrier(color_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    uint32_t width() const  { return width_; }
    uint32_t height() const { return height_; }

    VkImageView full_view() const { return full_view_; }
    coopa::gfx::TextureView full_view_typed() const { return coopa::gfx::detail::wrap(full_view_); }
    uint32_t max_mip_level() const { return mip_levels_ > 0 ? mip_levels_ - 1 : 0; }
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

        if (color_image_ != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator_.handle(), color_image_, allocation_);
            color_image_ = VK_NULL_HANDLE;
            allocation_ = VK_NULL_HANDLE;
        }
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;

    uint32_t width_      = 0;
    uint32_t height_     = 0;
    uint32_t mip_levels_ = 0;

    VkImage       color_image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_  = VK_NULL_HANDLE;
    VkImageView   full_view_   = VK_NULL_HANDLE;

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

#endif // GFXCOOPA_ENGINE_PASSES_SCENE_COLOR_MIP_PASS_H
