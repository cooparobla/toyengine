#include <toyengine/render/dir_shadow_cache.h>

#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/util/error.h>

#include <algorithm>

namespace toy {
namespace render {

DirShadowCache::DirShadowCache(coopa::gfx::core::Device& device,
                               coopa::gfx::memory::Allocator& allocator,
                               VkImage live_image, VkImageView live_view,
                               uint32_t width, uint32_t height)
    : device_(device), live_image_(live_image),
      width_(std::max(width, 16u)), height_(std::max(height, 16u))
{
    cache_ = std::make_unique<coopa::gfx::memory::Image>(
        device, allocator, width_, height_, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);

    // The live pass starts from TRANSFER_DST (begin_frame left it there, and the tile copies
    // wrote into it) and ends SHADER_READ for the lighting pass. The cache pass stays in
    // TRANSFER_SRC throughout: nothing samples it, the copies only read it.
    live_pass_  = make_pass_(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cache_pass_ = make_pass_(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    live_fb_    = make_fb_(live_pass_, live_view);
    cache_fb_   = make_fb_(cache_pass_, cache_->view());
}

DirShadowCache::~DirShadowCache() {
    VkDevice d = device_.handle();
    if (live_fb_)    vkDestroyFramebuffer(d, live_fb_, nullptr);
    if (cache_fb_)   vkDestroyFramebuffer(d, cache_fb_, nullptr);
    if (live_pass_)  vkDestroyRenderPass(d, live_pass_, nullptr);
    if (cache_pass_) vkDestroyRenderPass(d, cache_pass_, nullptr);
}

void DirShadowCache::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    // ONLY the cache image. The live atlas is not ours to clear: it is borrowed, it already
    // holds a frame the lighting pass may still be reading, and a transition out of
    // UNDEFINED would both discard that content and fail to order against those reads.
    // Its layout is handled entirely by begin_frame()'s SHADER_READ -> TRANSFER_DST barrier,
    // which is why the pipeline only starts caching on a frame after one that left the atlas
    // in SHADER_READ (see record_directional_shadow_()).
    clear_image_(cmd, cache_->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    initialized_ = true;
}

void DirShadowCache::begin_frame(coopa::gfx::command::CommandBuffer& cmd) const {
    barrier_(cmd, live_image_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
             VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void DirShadowCache::set_tile(coopa::gfx::command::CommandBuffer& cmd, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h) const {
    cmd.set_viewport(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(w), static_cast<float>(h));
    cmd.set_scissor(static_cast<int32_t>(x), static_cast<int32_t>(y), w, h);
}

void DirShadowCache::clear_tile(coopa::gfx::command::CommandBuffer& cmd, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h) const {
    VkClearAttachment att{};
    att.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    att.clearValue.depthStencil = {1.0f, 0};
    VkClearRect rect{};
    rect.rect.offset    = {static_cast<int32_t>(x), static_cast<int32_t>(y)};
    rect.rect.extent    = {w, h};
    rect.baseArrayLayer = 0;
    rect.layerCount     = 1;
    vkCmdClearAttachments(cmd.handle(), 1, &att, 1, &rect);
}

VkImageCopy DirShadowCache::tile_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    c.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    c.srcOffset      = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
    c.dstOffset      = c.srcOffset;
    c.extent         = {w, h, 1};
    return c;
}

void DirShadowCache::copy_levels(coopa::gfx::command::CommandBuffer& cmd,
                                 const std::vector<VkImageCopy>& regions) const {
    if (regions.empty()) return;
    vkCmdCopyImage(cmd.handle(), cache_->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   live_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   static_cast<uint32_t>(regions.size()), regions.data());
}

VkRenderPass DirShadowCache::make_pass_(VkImageLayout initial, VkImageLayout final_layout) {
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

    // Both directions: the pass may follow a transfer (the tile copies), a sample (last
    // frame's lighting) or another depth pass, and may be followed by a transfer or a sample.
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
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

VkFramebuffer DirShadowCache::make_fb_(VkRenderPass pass, VkImageView view) {
    VkFramebufferCreateInfo fb{};
    fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass      = pass;
    fb.attachmentCount = 1;
    fb.pAttachments    = &view;
    fb.width           = width_;
    fb.height          = height_;
    fb.layers          = 1;
    VkFramebuffer out = VK_NULL_HANDLE;
    GFX_VK_CHECK(vkCreateFramebuffer(device_.handle(), &fb, nullptr, &out));
    return out;
}

void DirShadowCache::begin_(coopa::gfx::command::CommandBuffer& cmd, VkRenderPass pass,
                            VkFramebuffer fb) const {
    VkRenderPassBeginInfo rp{};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = pass;
    rp.framebuffer       = fb;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {width_, height_};
    vkCmdBeginRenderPass(cmd.handle(), &rp, VK_SUBPASS_CONTENTS_INLINE);
}

void DirShadowCache::clear_image_(coopa::gfx::command::CommandBuffer& cmd, VkImage image,
                                  VkImageLayout to, VkAccessFlags to_access,
                                  VkPipelineStageFlags to_stage) const {
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

void DirShadowCache::barrier_(coopa::gfx::command::CommandBuffer& cmd, VkImage image,
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
