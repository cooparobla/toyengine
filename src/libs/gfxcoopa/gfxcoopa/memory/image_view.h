/**
 * @file image_view.h
 * @brief An owned view over part of an image -- a mip level, a layer, the whole chain.
 *
 * memory::Image creates one view covering itself, which is all a render target or an ordinary
 * texture ever needs. A second view is needed when one image is read two different ways at
 * once, and the compute mip chain is the case that forces it: the downsample binds level N-1
 * and level N as two separate single-mip storage images, while the shader that later samples
 * the result reads every level through one view. Those are three views over one image, and a
 * storage binding cannot use a view that spans levels.
 *
 * An ImageView borrows its image. It must not outlive it, exactly as a VkImageView must not
 * outlive its VkImage -- which is why the type is movable but not copyable, and why Image
 * vends it (Image::make_view()) rather than leaving callers to pair the two by hand.
 */

#ifndef COOPA_GFX_MEMORY_IMAGE_VIEW_H
#define COOPA_GFX_MEMORY_IMAGE_VIEW_H

#include <volk/volk.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/types/image_desc.h>
#include <gfxcoopa/types/image_handle.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace memory {

/**
 * @class ImageView
 * @brief RAII VkImageView over a borrowed image.
 */
class ImageView {
public:
    /**
     * @brief Creates a view over `image` as `desc` describes it.
     *
     * @param device The logical device.
     * @param image  The image to view; borrowed, and must outlive this view.
     * @param format The image's format, for the view to reinterpret or inherit.
     * @param desc   Which mips and layers, read as which kind.
     */
    ImageView(core::Device& device, ImageHandle image, Format format, const ImageViewDesc& desc)
        : device_(&device)
    {
        VkImageViewCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image            = detail::unwrap_image(image);
        info.viewType         = detail::to_vk_view_type(desc.kind);
        info.format           = detail::to_vk(desc.format == Format::Undefined ? format : desc.format);
        info.subresourceRange = detail::to_vk(desc.range, desc.aspect);
        GFX_VK_CHECK(vkCreateImageView(device_->handle(), &info, nullptr, &view_));
    }

    ~ImageView() {
        if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_->handle(), view_, nullptr);
    }

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    /** @brief Move constructor: transfers ownership, leaving `other` null. */
    ImageView(ImageView&& other) noexcept : device_(other.device_), view_(other.view_) {
        other.view_ = VK_NULL_HANDLE;
    }

    /** @brief Move assignment: destroys this view, then takes `other`'s. */
    ImageView& operator=(ImageView&& other) noexcept {
        if (this != &other) {
            if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_->handle(), view_, nullptr);
            device_ = other.device_;
            view_ = other.view_;
            other.view_ = VK_NULL_HANDLE;
        }
        return *this;
    }

    /** @brief The sealed identity a descriptor binds. */
    TextureView view_typed() const { return detail::wrap(view_); }

    /** @brief The raw handle, for gfxcoopa's own implementation. */
    VkImageView handle() const { return view_; }

private:
    core::Device* device_ = nullptr;             /**< Owning logical device (not owned). */
    VkImageView   view_   = VK_NULL_HANDLE;      /**< The Vulkan image view. */
};

} // namespace memory
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_MEMORY_IMAGE_VIEW_H
