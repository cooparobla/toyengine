#include <toyengine/render/local_shadow_atlas.h>

#include <toyengine/render/shadow_pass_desc.h>

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

namespace {

/**
 * @brief Clears one atlas image to the far plane and leaves it in `resting`.
 *
 * Undefined as the source layout because this runs once, before anything has written the image,
 * so there are no contents to preserve.
 */
void clear_to_far_plane(coopa::gfx::command::CommandBuffer& cmd, coopa::gfx::memory::Image& image,
                        coopa::gfx::TextureUsage resting) {
    using coopa::gfx::ImageAspect;
    using coopa::gfx::ImageBarrier;
    using coopa::gfx::TextureUsage;
    cmd.barrier(image, ImageBarrier{TextureUsage::Undefined, TextureUsage::TransferDst,
                                    ImageAspect::Depth});
    cmd.clear_depth(image);
    cmd.barrier(image, ImageBarrier{TextureUsage::TransferDst, resting, ImageAspect::Depth});
}

} // namespace

LocalShadowAtlas::LocalShadowAtlas(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                 uint32_t size, bool static_cache)
    : device_(device), size_(std::max(size, 16u)), caching_(static_cache)
{
    using coopa::gfx::ImageUsage;
    using coopa::gfx::TextureUsage;
    const coopa::gfx::Extent2D extent{size_, size_};

    live_ = std::make_unique<coopa::gfx::memory::Image>(
        device, allocator, size_, size_, coopa::gfx::Format::D32_Sfloat,
        ImageUsage::DepthAttachment | ImageUsage::Sampled | ImageUsage::TransferDst
            | ImageUsage::TransferSrc);
    live_pass_ = std::make_unique<coopa::gfx::pipeline::RenderPass>(
        device, shadow_atlas_pass_desc(TextureUsage::TransferDst, TextureUsage::ShaderRead));
    live_fb_ = std::make_unique<coopa::gfx::pipeline::Framebuffer>(
        device, *live_pass_, live_->view_typed(), extent);

    if (caching_) {
        cache_ = std::make_unique<coopa::gfx::memory::Image>(
            device, allocator, size_, size_, coopa::gfx::Format::D32_Sfloat,
            ImageUsage::DepthAttachment | ImageUsage::TransferDst | ImageUsage::TransferSrc);
        // Starts AND ends TransferSrc: the cache is only ever drawn into and copied out of, so
        // it is never presented and never sampled -- the case AttachmentUse cannot name.
        cache_pass_ = std::make_unique<coopa::gfx::pipeline::RenderPass>(
            device, shadow_atlas_pass_desc(TextureUsage::TransferSrc, TextureUsage::TransferSrc));
        cache_fb_ = std::make_unique<coopa::gfx::pipeline::Framebuffer>(
            device, *cache_pass_, cache_->view_typed(), extent);
    }
}

void LocalShadowAtlas::initialize(coopa::gfx::command::CommandBuffer& cmd) {
    if (initialized_) return;
    initialized_ = true;
    // The live atlas rests shader-readable: its descriptor is bound from frame 0 whether or not
    // any light casts. The cache rests transfer-readable, which is what copy_static() reads and
    // what its own render pass declares as both initial and final layout.
    clear_to_far_plane(cmd, *live_, coopa::gfx::TextureUsage::ShaderRead);
    if (caching_) clear_to_far_plane(cmd, *cache_, coopa::gfx::TextureUsage::TransferSrc);
}

void LocalShadowAtlas::begin_frame(coopa::gfx::command::CommandBuffer& cmd) const {
    cmd.barrier(*live_, coopa::gfx::ImageBarrier{
        coopa::gfx::TextureUsage::ShaderRead, coopa::gfx::TextureUsage::TransferDst,
        coopa::gfx::ImageAspect::Depth});
}

void LocalShadowAtlas::set_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const {
    cmd.set_viewport(static_cast<float>(r.x), static_cast<float>(r.y),
                     static_cast<float>(r.w), static_cast<float>(r.h));
    cmd.set_scissor(static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), r.w, r.h);
}

void LocalShadowAtlas::clear_tile(coopa::gfx::command::CommandBuffer& cmd, const AtlasRect& r) const {
    cmd.clear_depth_rect(coopa::gfx::ImageRegion{static_cast<int32_t>(r.x), static_cast<int32_t>(r.y),
                                                r.w, r.h});
}

void LocalShadowAtlas::copy_static(coopa::gfx::command::CommandBuffer& cmd, const std::vector<AtlasRect>& rects) const {
    if (!caching_ || rects.empty()) return;
    std::vector<coopa::gfx::ImageCopy> regions;
    regions.reserve(rects.size());
    for (const AtlasRect& r : rects) {
        regions.push_back(coopa::gfx::ImageCopy::tile(static_cast<int32_t>(r.x),
                                                      static_cast<int32_t>(r.y), r.w, r.h));
    }
    cmd.copy_image(cache_->handle_typed(), live_->handle_typed(), coopa::gfx::ImageAspect::Depth,
                   regions.data(), static_cast<uint32_t>(regions.size()));
}

} // namespace render
} // namespace toy
