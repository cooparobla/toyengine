#include <toyengine/render/dir_shadow_cache.h>

#include <toyengine/render/shadow_pass_desc.h>

#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/util/error.h>

#include <algorithm>

namespace toy {
namespace render {

DirShadowCache::DirShadowCache(coopa::gfx::core::Device& device,
                               coopa::gfx::memory::Allocator& allocator,
                               coopa::gfx::ImageHandle live_image, coopa::gfx::TextureView live_view,
                               uint32_t width, uint32_t height)
    : device_(device), live_image_(live_image),
      width_(std::max(width, 16u)), height_(std::max(height, 16u))
{
    using coopa::gfx::ImageUsage;
    using coopa::gfx::TextureUsage;
    const coopa::gfx::Extent2D extent{width_, height_};

    cache_ = std::make_unique<coopa::gfx::memory::Image>(
        device, allocator, width_, height_, coopa::gfx::Format::D32_Sfloat,
        ImageUsage::DepthAttachment | ImageUsage::TransferDst | ImageUsage::TransferSrc);

    // The live pass starts from TransferDst (begin_frame left it there, and the tile copies
    // wrote into it) and ends ShaderRead for the lighting pass. The cache pass stays in
    // TransferSrc throughout: nothing samples it, the copies only read it.
    live_pass_  = std::make_unique<coopa::gfx::pipeline::RenderPass>(
        device, shadow_atlas_pass_desc(TextureUsage::TransferDst, TextureUsage::ShaderRead));
    cache_pass_ = std::make_unique<coopa::gfx::pipeline::RenderPass>(
        device, shadow_atlas_pass_desc(TextureUsage::TransferSrc, TextureUsage::TransferSrc));
    live_fb_    = std::make_unique<coopa::gfx::pipeline::Framebuffer>(
        device, *live_pass_, live_view, extent);
    cache_fb_   = std::make_unique<coopa::gfx::pipeline::Framebuffer>(
        device, *cache_pass_, cache_->view_typed(), extent);
}

void DirShadowCache::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    using coopa::gfx::ImageAspect;
    using coopa::gfx::ImageBarrier;
    using coopa::gfx::TextureUsage;
    // ONLY the cache image. The live atlas is not ours to clear: it is borrowed, it already
    // holds a frame the lighting pass may still be reading, and a transition out of Undefined
    // would both discard that content and fail to order against those reads. Its layout is
    // handled entirely by begin_frame()'s ShaderRead -> TransferDst barrier, which is why the
    // pipeline only starts caching on a frame after one that left the atlas in ShaderRead
    // (see record_directional_shadow_()).
    cmd.barrier(*cache_, ImageBarrier{TextureUsage::Undefined, TextureUsage::TransferDst,
                                      ImageAspect::Depth});
    cmd.clear_depth(*cache_);
    cmd.barrier(*cache_, ImageBarrier{TextureUsage::TransferDst, TextureUsage::TransferSrc,
                                      ImageAspect::Depth});
    initialized_ = true;
}

void DirShadowCache::begin_frame(coopa::gfx::command::CommandBuffer& cmd) const {
    // Untracked barrier(), not transition(): the live atlas is borrowed and ShadowMapTarget's
    // own render pass moves its layout behind gfxcoopa's back, so its current_usage() record
    // would be a lie. Both layouts are therefore stated here.
    cmd.barrier(live_image_, coopa::gfx::ImageBarrier{
        coopa::gfx::TextureUsage::ShaderRead, coopa::gfx::TextureUsage::TransferDst,
        coopa::gfx::ImageAspect::Depth});
}

void DirShadowCache::set_tile(coopa::gfx::command::CommandBuffer& cmd, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h) const {
    cmd.set_viewport(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(w), static_cast<float>(h));
    cmd.set_scissor(static_cast<int32_t>(x), static_cast<int32_t>(y), w, h);
}

void DirShadowCache::clear_tile(coopa::gfx::command::CommandBuffer& cmd, uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h) const {
    cmd.clear_depth_rect(coopa::gfx::ImageRegion{static_cast<int32_t>(x), static_cast<int32_t>(y), w, h});
}

coopa::gfx::ImageCopy DirShadowCache::tile_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return coopa::gfx::ImageCopy::tile(static_cast<int32_t>(x), static_cast<int32_t>(y), w, h);
}

void DirShadowCache::copy_levels(coopa::gfx::command::CommandBuffer& cmd,
                                 const std::vector<coopa::gfx::ImageCopy>& regions) const {
    if (regions.empty()) return;
    cmd.copy_image(cache_->handle_typed(), live_image_, coopa::gfx::ImageAspect::Depth,
                   regions.data(), static_cast<uint32_t>(regions.size()));
}

} // namespace render
} // namespace toy
