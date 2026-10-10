#include <toyengine/render/local_shadow_atlas.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/util/error.h>
#include <volk/volk.h>

namespace toy {
namespace render {

std::vector<AtlasRect> pack_shadow_atlas(const std::vector<std::pair<uint32_t, uint32_t>>& sizes,
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

glm::mat4 local_shadow_point_face_view(uint32_t face, const glm::vec3& pos) {
    static const glm::vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 ups[6]  = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    return glm::lookAt(pos, pos + dirs[face], ups[face]);
}

LocalShadowAtlas::LocalShadowAtlas(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
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

LocalShadowAtlas::~LocalShadowAtlas() {
    VkDevice d = device_.handle();
    if (live_fb_)    vkDestroyFramebuffer(d, live_fb_, nullptr);
    if (cache_fb_)   vkDestroyFramebuffer(d, cache_fb_, nullptr);
    if (live_pass_)  vkDestroyRenderPass(d, live_pass_, nullptr);
    if (cache_pass_) vkDestroyRenderPass(d, cache_pass_, nullptr);
}

void LocalShadowAtlas::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    clear_image_(cmd, live_->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (caching_) {
        clear_image_(cmd, cache_->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    initialized_ = true;
}

void LocalShadowAtlas::begin_frame(coopa::gfx::command::CommandBuffer& cmd) const {
    barrier_(cmd, live_->handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void LocalShadowAtlas::set_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const {
    cmd.set_viewport(static_cast<float>(r.x), static_cast<float>(r.y),
                     static_cast<float>(r.w), static_cast<float>(r.h));
    cmd.set_scissor(static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), r.w, r.h);
}

void LocalShadowAtlas::clear_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const {
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

void LocalShadowAtlas::copy_static(coopa::gfx::command::CommandBuffer& cmd, const std::vector<AtlasRect>& rects) const {
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

VkRenderPass LocalShadowAtlas::make_pass_(VkImageLayout initial, VkImageLayout final_layout) {
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

VkFramebuffer LocalShadowAtlas::make_fb_(VkRenderPass pass, VkImageView view) {
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

void LocalShadowAtlas::begin_(coopa::gfx::command::CommandBuffer& cmd, VkRenderPass pass, VkFramebuffer fb) const {
    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = pass;
    rp.framebuffer       = fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {size_, size_};
    vkCmdBeginRenderPass(cmd.handle(), &rp, VK_SUBPASS_CONTENTS_INLINE);
}

void LocalShadowAtlas::clear_image_(coopa::gfx::command::CommandBuffer& cmd, VkImage image, VkImageLayout to,
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

void LocalShadowAtlas::barrier_(coopa::gfx::command::CommandBuffer& cmd, VkImage image,
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

} // namespace render
} // namespace toy
