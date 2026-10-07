/**
 * @file local_shadow_atlas.h
 * @brief The point/spot shadow atlas: every shadowed local light's depth tiles in one image,
 *        plus an optional static-caster cache copied in each frame.
 */
#ifndef TOYENGINE_RENDER_LOCAL_SHADOW_ATLAS_H
#define TOYENGINE_RENDER_LOCAL_SHADOW_ATLAS_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/util/error.h>

namespace toy {
namespace render {

/** @brief A tile rectangle in atlas texels. */
struct AtlasRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const AtlasRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

/**
 * @brief Shelf-packs rectangles into a square atlas, in the order given.
 *
 * Each item goes on the first open shelf with enough height and remaining width, else on a new
 * shelf below the last. Deterministic for a given input order, which is what keeps a light's
 * tile (and so its cached static depth) in place frame to frame. An item that fits nowhere gets
 * w = 0.
 */
inline std::vector<AtlasRect> pack_shadow_atlas(const std::vector<std::pair<uint32_t, uint32_t>>& sizes,
                                                uint32_t atlas) {
    struct Shelf { uint32_t y, h, used; };
    std::vector<Shelf> shelves;
    uint32_t next_y = 0;
    std::vector<AtlasRect> out(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        const uint32_t w = sizes[i].first, h = sizes[i].second;
        if (w == 0 || h == 0 || w > atlas || h > atlas) continue;
        bool placed = false;
        for (Shelf& s : shelves) {
            if (s.h >= h && atlas - s.used >= w) {
                out[i] = {s.used, s.y, w, h};
                s.used += w;
                placed = true;
                break;
            }
        }
        if (!placed && next_y + h <= atlas) {
            shelves.push_back({next_y, h, w});
            out[i] = {0, next_y, w, h};
            next_y += h;
        }
    }
    return out;
}

/// The six cube-face directions and up vectors, in the face order gfx/local_shadow.glsl's
/// gfx_local_shadow_face() selects by major axis: +X, -X, +Y, -Y, +Z, -Z.
inline glm::mat4 local_shadow_point_face_view(uint32_t face, const glm::vec3& pos) {
    static const glm::vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 ups[6]  = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    return glm::lookAt(pos, pos + dirs[face], ups[face]);
}

/**
 * @class LocalShadowAtlas
 * @brief Owns the local-light shadow atlas (D32, sampled by gfx/local_shadow.glsl) and, when
 *        caching, a second atlas of the same layout holding only STATIC casters.
 *
 * Per frame (see PixelRenderPipeline::record_local_shadows_()):
 *  1. begin_frame(): the live atlas goes from shader-read to transfer-dst.
 *  2. Lights whose static content changed re-render it into the cache atlas
 *     (begin_static_pass / clear_tile / draws / end_static_pass).
 *  3. copy_static(): every active light's tiles are copied cache -> live.
 *  4. begin_dynamic_pass(): moving casters (and everything, with caching off) draw on top;
 *     the pass ends in shader-read for the lighting pass.
 *
 * Both render passes LOAD their attachment and are render-pass-compatible with
 * ShadowMapTarget's directional pass (one D32 attachment, one subpass), so the existing
 * directional shadow pipelines and their per-material variants draw into either unchanged.
 */
class LocalShadowAtlas {
public:
    LocalShadowAtlas(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                     uint32_t size, bool static_cache)
        : device_(device), size_(std::max(size, 16u)), caching_(static_cache)
    {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        live_ = std::make_unique<coopa::gfx::memory::Image>(
            device, allocator, size_, size_, VK_FORMAT_D32_SFLOAT, usage, VK_IMAGE_ASPECT_DEPTH_BIT);
        live_pass_ = make_pass_(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        live_fb_   = make_fb_(live_pass_, live_->view());
        if (caching_) {
            cache_ = std::make_unique<coopa::gfx::memory::Image>(
                device, allocator, size_, size_, VK_FORMAT_D32_SFLOAT,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            cache_pass_ = make_pass_(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cache_fb_   = make_fb_(cache_pass_, cache_->view());
        }
    }

    ~LocalShadowAtlas() {
        VkDevice d = device_.handle();
        if (live_fb_)    vkDestroyFramebuffer(d, live_fb_, nullptr);
        if (cache_fb_)   vkDestroyFramebuffer(d, cache_fb_, nullptr);
        if (live_pass_)  vkDestroyRenderPass(d, live_pass_, nullptr);
        if (cache_pass_) vkDestroyRenderPass(d, cache_pass_, nullptr);
    }

    LocalShadowAtlas(const LocalShadowAtlas&) = delete;
    LocalShadowAtlas& operator=(const LocalShadowAtlas&) = delete;

    uint32_t size() const { return size_; }
    bool     caching() const { return caching_; }
    VkImageView view() const { return live_->view(); }
    coopa::gfx::TextureView view_typed() const { return live_->view_typed(); }

    /**
     * @brief Once, before the atlas is first sampled: clears both images to the far plane and
     *        leaves the live one shader-readable (its descriptor is bound from frame 0, whether
     *        or not any light casts) and the cache transfer-readable.
     */
    void initialize(coopa::gfx::command::CommandBuffer& cmd) {
        if (initialized_) return;
        clear_image_(cmd, live_->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        if (caching_) {
            clear_image_(cmd, cache_->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        initialized_ = true;
    }

    /** @brief Live atlas: shader-read -> transfer-dst, ready for the cache copy / live pass. */
    void begin_frame(coopa::gfx::command::CommandBuffer& cmd) const {
        barrier_(cmd, live_->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);
    }

    void begin_static_pass(coopa::gfx::command::CommandBuffer& cmd) const { begin_(cmd, cache_pass_, cache_fb_); }
    void end_static_pass(coopa::gfx::command::CommandBuffer& cmd) const { cmd.end_render_pass(); }
    void begin_dynamic_pass(coopa::gfx::command::CommandBuffer& cmd) const { begin_(cmd, live_pass_, live_fb_); }
    void end_dynamic_pass(coopa::gfx::command::CommandBuffer& cmd) const { cmd.end_render_pass(); }

    /** @brief Viewport + scissor to one tile (positive height, matching the other shadow maps). */
    void set_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const {
        cmd.set_viewport(static_cast<float>(r.x), static_cast<float>(r.y),
                         static_cast<float>(r.w), static_cast<float>(r.h));
        cmd.set_scissor(static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), r.w, r.h);
    }

    /** @brief Inside an open pass: resets one rectangle to the far plane. */
    void clear_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const {
        VkClearAttachment att{};
        att.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        att.clearValue.depthStencil = {1.0f, 0};
        VkClearRect rect{};
        rect.rect.offset = {static_cast<int32_t>(r.x), static_cast<int32_t>(r.y)};
        rect.rect.extent = {r.w, r.h};
        rect.baseArrayLayer = 0;
        rect.layerCount     = 1;
        vkCmdClearAttachments(cmd.handle(), 1, &att, 1, &rect);
    }

    /** @brief Copies each rectangle of the static cache into the live atlas (outside any pass). */
    void copy_static(coopa::gfx::command::CommandBuffer& cmd, const std::vector<AtlasRect>& rects) const {
        if (!caching_ || rects.empty()) return;
        std::vector<VkImageCopy> regions;
        regions.reserve(rects.size());
        for (const AtlasRect& r : rects) {
            VkImageCopy c{};
            c.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            c.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            c.srcOffset = {static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), 0};
            c.dstOffset = c.srcOffset;
            c.extent    = {r.w, r.h, 1};
            regions.push_back(c);
        }
        vkCmdCopyImage(cmd.handle(), cache_->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       live_->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       static_cast<uint32_t>(regions.size()), regions.data());
    }

private:
    /// One D32 attachment, LOAD/STORE, with explicit dependencies: in from earlier transfers
    /// (the cache copy, init clears) and depth writes; out to transfer reads (the copy) and
    /// fragment-shader reads (lighting, volumetrics).
    VkRenderPass make_pass_(VkImageLayout initial, VkImageLayout final_layout) {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_D32_SFLOAT;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = initial;
        att.finalLayout    = final_layout;

        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.pDepthStencilAttachment = &ref;

        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments    = &att;
        info.subpassCount    = 1;
        info.pSubpasses      = &sub;
        info.dependencyCount = 2;
        info.pDependencies   = deps;
        VkRenderPass pass = VK_NULL_HANDLE;
        GFX_VK_CHECK(vkCreateRenderPass(device_.handle(), &info, nullptr, &pass));
        return pass;
    }

    VkFramebuffer make_fb_(VkRenderPass pass, VkImageView view) {
        VkFramebufferCreateInfo fb{};
        fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass      = pass;
        fb.attachmentCount = 1;
        fb.pAttachments    = &view;
        fb.width           = size_;
        fb.height          = size_;
        fb.layers          = 1;
        VkFramebuffer out = VK_NULL_HANDLE;
        GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb, nullptr, &out));
        return out;
    }

    void begin_(coopa::gfx::command::CommandBuffer& cmd, VkRenderPass pass, VkFramebuffer fb) const {
        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = pass;
        rp.framebuffer       = fb;
        rp.renderArea.offset = {0, 0};
        rp.renderArea.extent = {size_, size_};
        vkCmdBeginRenderPass(cmd.handle(), &rp, VK_SUBPASS_CONTENTS_INLINE);
    }

    void clear_image_(coopa::gfx::command::CommandBuffer& cmd, VkImage image, VkImageLayout to,
                      VkAccessFlags to_access, VkPipelineStageFlags to_stage) const {
        barrier_(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearDepthStencilValue far_plane{1.0f, 0};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd.handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &far_plane, 1, &range);
        barrier_(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to, VK_ACCESS_TRANSFER_WRITE_BIT,
                 to_access, VK_PIPELINE_STAGE_TRANSFER_BIT, to_stage);
    }

    static void barrier_(coopa::gfx::command::CommandBuffer& cmd, VkImage image,
                         VkImageLayout from, VkImageLayout to,
                         VkAccessFlags src_access, VkAccessFlags dst_access,
                         VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = from;
        b.newLayout           = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = image;
        b.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = src_access;
        b.dstAccessMask       = dst_access;
        vkCmdPipelineBarrier(cmd.handle(), src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    coopa::gfx::core::Device& device_;
    uint32_t size_;
    bool     caching_;
    bool     initialized_ = false;
    std::unique_ptr<coopa::gfx::memory::Image> live_;
    std::unique_ptr<coopa::gfx::memory::Image> cache_;
    VkRenderPass  live_pass_  = VK_NULL_HANDLE;
    VkRenderPass  cache_pass_ = VK_NULL_HANDLE;
    VkFramebuffer live_fb_    = VK_NULL_HANDLE;
    VkFramebuffer cache_fb_   = VK_NULL_HANDLE;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_LOCAL_SHADOW_ATLAS_H
