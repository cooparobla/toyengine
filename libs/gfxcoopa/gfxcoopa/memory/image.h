/**
 * @file image.h
 * @brief 2D Vulkan image and image view management via VMA.
 *
 * Provides a 2D Image backed by a VMA allocation, automatically creating a
 * matching VkImageView covering it. Analogous to OpenGL's texture objects.
 *
 * Image owns storage and identity only. Recording work against it lives
 * elsewhere: command::CommandBuffer::transition() for layout changes, and
 * memory::upload_image_2d() (memory/image_upload.h) for staged texel upload.
 */

#ifndef COOPA_GFX_MEMORY_IMAGE_H
#define COOPA_GFX_MEMORY_IMAGE_H

#include <volk/volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

#include <cstring>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/util/format.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace memory {

/**
 * @class Image
 * @brief RAII 2D Vulkan image with an auto-created VkImageView.
 *
 * Wraps VkImage + VmaAllocation + VkImageView into a single object, and
 * tracks the TextureUsage it was last transitioned to (see current_usage()).
 *
 * Single-layer 2D, with an optional mip chain (`mip_levels` at construction; the view covers
 * every level and transition() moves them together). Cube faces and array layers are built
 * directly against Vulkan by the engine targets that need them.
 */
class Image {
public:
    /**
     * @brief Creates a 2D VkImage with a VMA-managed allocation and a VkImageView.
     *
     * @param device        The logical device.
     * @param allocator     The VMA allocator.
     * @param width         Image width in pixels.
     * @param height        Image height in pixels.
     * @param format        Pixel format.
     * @param usage         VkImageUsageFlags (sampled, color attachment, transfer dst, etc.).
     * @param aspect_mask   Which aspects to expose in the image view (color or depth).
     * @param memory_usage  VMA memory usage hint.
     */
    Image(core::Device&      device,
          Allocator&         allocator,
          uint32_t           width,
          uint32_t           height,
          VkFormat           format,
          VkImageUsageFlags  usage,
          VkImageAspectFlags aspect_mask  = VK_IMAGE_ASPECT_COLOR_BIT,
          VmaMemoryUsage     memory_usage = VMA_MEMORY_USAGE_AUTO,
          VkSampleCountFlagBits samples   = VK_SAMPLE_COUNT_1_BIT,
          uint32_t           mip_levels   = 1)
        : device_(device), allocator_(allocator.handle()),
          width_(width), height_(height), format_(format),
          mip_levels_(mip_levels > 0 ? mip_levels : 1)
    {
        create_image(usage, memory_usage, samples);
        create_view(aspect_mask);
    }

    /**
     * @brief Creates a 2D image using gfxcoopa's sealed Format/ImageUsage/
     * MemoryResidency/SampleCount vocabulary instead of raw Vulkan/VMA types.
     *
     * The aspect mask (color vs. depth[+stencil]) is derived from `format`,
     * so a depth image gets a depth aspect without the caller having to know
     * to ask for one.
     *
     * @param device    The logical device.
     * @param allocator The VMA allocator.
     * @param width     Image width in pixels.
     * @param height    Image height in pixels.
     * @param format    Pixel format.
     * @param usage     Every role this image may be used in (bitmask).
     * @param residency Where the memory lives; GpuOnly for render targets.
     * @param samples   MSAA sample count.
     */
    Image(core::Device& device, Allocator& allocator, uint32_t width, uint32_t height,
          Format format, ImageUsage usage,
          MemoryResidency residency = MemoryResidency::GpuOnly,
          SampleCount samples = SampleCount::X1)
        : Image(device, allocator, width, height, detail::to_vk(format), detail::to_vk(usage),
                detail::aspect_mask_for(format), detail::to_vma(residency).usage, detail::to_vk(samples))
    {}

    /**
     * @brief Destroys the image view, then the image and its allocation.
     */
    ~Image() {
        if (view_  != VK_NULL_HANDLE) vkDestroyImageView(device_.handle(), view_,  nullptr);
        if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);
    }

    /// @brief Non-copyable.
    Image(const Image&) = delete;
    /// @brief Non-copyable.
    Image& operator=(const Image&) = delete;

    // --- Accessors ---

    /**
     * @brief Returns the underlying VkImage handle.
     * @return Raw VkImage.
     */
    VkImage handle() const { return image_; }

    /**
     * @brief Returns the VkImageView covering the full image.
     * @return Raw VkImageView.
     */
    VkImageView view() const { return view_; }

    /**
     * @brief Returns a sealed TextureView identity for this image's view.
     *
     * The sibling of view() that public gfxcoopa signatures should return
     * going forward -- see gfxcoopa/types/texture_view.h for why.
     */
    TextureView view_typed() const { return detail::wrap(view_); }

    /**
     * @brief Returns the image format.
     * @return VkFormat of this image.
     */
    VkFormat format() const { return format_; }

    /// @brief Returns the sealed Format sibling of format().
    Format format_typed() const { return detail::from_vk(format_); }

    /**
     * @brief Returns the image width in pixels.
     * @return Width in pixels.
     */
    uint32_t width()  const { return width_; }

    /**
     * @brief Returns the image height in pixels.
     * @return Height in pixels.
     */
    uint32_t height() const { return height_; }

    /** @brief Number of mip levels (1 for the single-mip common case). */
    uint32_t mip_levels() const { return mip_levels_; }

    /**
     * @brief The full mip chain length for a `width` x `height` image: every level down to 1x1.
     * @param width  Level-0 width in pixels.
     * @param height Level-0 height in pixels.
     * @return floor(log2(max(width, height))) + 1.
     */
    static uint32_t full_mip_count(uint32_t width, uint32_t height) {
        uint32_t levels = 1;
        for (uint32_t extent = width > height ? width : height; extent > 1; extent >>= 1) ++levels;
        return levels;
    }

    /**
     * @brief Returns the TextureUsage this image was last transitioned to,
     * or TextureUsage::Undefined if it has never been transitioned.
     *
     * This is what lets CommandBuffer::transition() take only a destination
     * usage: the Image tracks its own, so a caller never has to separately
     * track (and risk getting wrong) what layout it was last left in.
     *
     * Only CommandBuffer::transition() and mark_transitioned() update this.
     * A render pass's automatic attachment transition does NOT, so an image
     * used as a render target carries a stale reading until something calls
     * one of those two -- see util/image_readback.h, which corrects for it.
     */
    TextureUsage current_usage() const { return current_usage_; }

    /**
     * @brief Records that this image has been transitioned to `usage` by
     * some means OTHER than command::CommandBuffer::transition() (which
     * calls this internally already).
     *
     * Advanced/internal: only needed by code that records its own raw
     * `vkCmdPipelineBarrier` outside CommandBuffer::transition() -- e.g.
     * memory::upload_image_2d(). Ordinary application code should never
     * need this; call transition() instead, which stays in sync on its own.
     *
     * @param usage The usage this image has actually just been left in.
     */
    void mark_transitioned(TextureUsage usage) { current_usage_ = usage; }

private:

    /**
     * @brief Creates the VkImage and VMA allocation.
     * @param usage        Image usage flags.
     * @param memory_usage VMA memory usage hint.
     */
    void create_image(VkImageUsageFlags usage, VmaMemoryUsage memory_usage, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
        VkImageCreateInfo image_info{};
        image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType     = VK_IMAGE_TYPE_2D;
        image_info.extent.width  = width_;
        image_info.extent.height = height_;
        image_info.extent.depth  = 1;
        image_info.mipLevels     = mip_levels_;
        image_info.arrayLayers   = 1;
        image_info.format        = format_;
        image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.usage         = usage;
        image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        image_info.samples       = samples;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = memory_usage;

        VkResult result = vmaCreateImage(allocator_, &image_info, &alloc_info,
                                         &image_, &allocation_, nullptr);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("[gfxcoopa] vmaCreateImage failed: " +
                                     util::vk_result_string(result));
        }
    }

    /**
     * @brief Creates a VkImageView for the full image.
     * @param aspect_mask Which aspect(s) to expose in the view.
     */
    void create_view(VkImageAspectFlags aspect_mask) {
        VkImageViewCreateInfo view_info{};
        view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image                           = image_;
        view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format                          = format_;
        view_info.subresourceRange.aspectMask     = aspect_mask;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = mip_levels_;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = 1;

        GFX_VK_CHECK(vkCreateImageView(device_.handle(), &view_info, nullptr, &view_));
    }

    core::Device& device_;                        /**< Owning logical device (not owned). */
    VmaAllocator  allocator_ = VK_NULL_HANDLE;    /**< VMA allocator handle (not owned). */
    VkImage       image_     = VK_NULL_HANDLE;    /**< The Vulkan image. */
    VmaAllocation allocation_ = VK_NULL_HANDLE;   /**< VMA allocation for the image memory. */
    VkImageView   view_      = VK_NULL_HANDLE;    /**< Image view covering the full image. */
    uint32_t      width_     = 0;                 /**< Image width in pixels. */
    uint32_t      height_    = 0;                 /**< Image height in pixels. */
    uint32_t      mip_levels_ = 1;                /**< Mip chain length; 1 = no chain. */
    VkFormat      format_    = VK_FORMAT_UNDEFINED;/**< Pixel format. */
    TextureUsage  current_usage_ = TextureUsage::Undefined; /**< See current_usage(). */
};

} // namespace memory
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_MEMORY_IMAGE_H
