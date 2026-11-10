/**
 * @file swapchain.h
 * @brief Vulkan swapchain management including image views and resize support.
 *
 * The Swapchain manages the ring of presentable images, selects the optimal
 * surface format and present mode, and creates VkImageViews for each image.
 * It exposes recreate() for transparent window resize handling.
 *
 * Analogous to the double/triple-buffered framebuffer in OpenGL.
 */

#ifndef COOPA_GFX_CORE_SWAPCHAIN_H
#define COOPA_GFX_CORE_SWAPCHAIN_H

#include <volk/volk.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/types/clear.h>   // Extent2D
#include <gfxcoopa/core/surface.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace core {

/**
 * @class Swapchain
 * @brief RAII wrapper around VkSwapchainKHR and its associated image views.
 *
 * On construction, selects:
 * - **Format**: VK_FORMAT_B8G8R8A8_SRGB / VK_COLOR_SPACE_SRGB_NONLINEAR_KHR preferred.
 * - **Present mode**: MAILBOX (uncapped, lowest latency) when vsync=false; FIFO otherwise.
 * - **Extent**: clamped to surface capabilities, respecting HiDPI framebuffer sizes.
 */
class Swapchain {
public:
    /**
     * @brief Creates the swapchain and all associated image views.
     * @param device  The logical device.
     * @param surface The window surface.
     * @param width   Initial framebuffer width in pixels.
     * @param height  Initial framebuffer height in pixels.
     * @param vsync   If true, selects FIFO (vsync). If false, prefers MAILBOX.
     */
    Swapchain(Device& device, const Surface& surface,
              uint32_t width, uint32_t height, bool vsync = true)
        : device_(device), surface_(surface), vsync_(vsync)
    {
        create(width, height);
    }

    /**
     * @brief Destroys the swapchain and all image views.
     */
    ~Swapchain() { destroy(); }

    /// @brief Non-copyable.
    Swapchain(const Swapchain&) = delete;
    /// @brief Non-copyable.
    Swapchain& operator=(const Swapchain&) = delete;

    // --- Accessors ---

    /**
     * @brief Returns the pixel format of the swapchain images.
     * @return VkFormat used for all swapchain images.
     */
    VkFormat image_format() const { return image_format_; }

    /**
     * @brief Returns the current render resolution.
     * @return VkExtent2D with width and height.
     */
    VkExtent2D extent() const { return extent_; }
    /// @brief Sealed sibling of extent(), so a consumer never names VkExtent2D.
    Extent2D extent_typed() const { return Extent2D{extent_.width, extent_.height}; }

    /**
     * @brief Returns the number of swapchain images.
     * @return Image count (typically 2 or 3 for double/triple buffering).
     */
    uint32_t image_count() const { return static_cast<uint32_t>(image_views_.size()); }

    /**
     * @brief Returns all image views, one per swapchain image.
     * @return Const reference to the image view vector.
     */
    const std::vector<VkImageView>& image_views() const { return image_views_; }

    /**
     * @brief Returns the raw VkSwapchainKHR handle.
     * @return Raw VkSwapchainKHR.
     */
    VkSwapchainKHR handle() const { return swapchain_; }

    // --- Resize ---

    /**
     * @brief Destroys and recreates the swapchain at the new framebuffer size.
     *
     * Call this when VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR is
     * returned from vkAcquireNextImageKHR or vkQueuePresentKHR.
     *
     * @param width  New framebuffer width in pixels.
     * @param height New framebuffer height in pixels.
     */
    void recreate(uint32_t width, uint32_t height) {
        // Wait for the device to be idle before destroying resources.
        device_.wait_idle();
        destroy();
        create(width, height);
    }

private:
    /**
     * @brief Creates the VkSwapchainKHR and its image views.
     * @param width  Desired framebuffer width.
     * @param height Desired framebuffer height.
     */
    void create(uint32_t width, uint32_t height) {
        SwapchainSupportDetails support = surface_.query_support(device_.physical());

        VkSurfaceFormatKHR surface_format = choose_format(support.formats);
        VkPresentModeKHR   present_mode   = choose_present_mode(support.present_modes);
        VkExtent2D         extent         = choose_extent(support.capabilities, width, height);

        // Request one more image than minimum for less driver latency.
        uint32_t image_count = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0) {
            image_count = std::min(image_count, support.capabilities.maxImageCount);
        }

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface          = surface_.handle();
        create_info.minImageCount    = image_count;
        create_info.imageFormat      = surface_format.format;
        create_info.imageColorSpace  = surface_format.colorSpace;
        create_info.imageExtent      = extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        QueueFamilyIndices indices = device_.queue_family_indices();
        uint32_t family_indices[] = {
            indices.graphics.value(),
            indices.present.value()
        };

        if (indices.graphics.value() != indices.present.value()) {
            // Different queue families — use concurrent sharing.
            create_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
            create_info.queueFamilyIndexCount = 2;
            create_info.pQueueFamilyIndices   = family_indices;
        } else {
            // Same family — exclusive mode for best performance.
            create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        create_info.preTransform   = support.capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode    = present_mode;
        create_info.clipped        = VK_TRUE;
        create_info.oldSwapchain   = VK_NULL_HANDLE;

        GFX_VK_CHECK(vkCreateSwapchainKHR(device_.handle(), &create_info, nullptr, &swapchain_));

        // Retrieve the swapchain images (driver may give more than we requested).
        uint32_t actual_count = 0;
        vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &actual_count, nullptr);
        images_.resize(actual_count);
        vkGetSwapchainImagesKHR(device_.handle(), swapchain_, &actual_count, images_.data());

        image_format_ = surface_format.format;
        extent_       = extent;

        // Once per process, not per resize: which mode a run got decides whether presenting can
        // block the frame loop at all, and a run that hitches is the first thing to check.
        static bool mode_logged = false;
        if (!mode_logged) {
            mode_logged = true;
            const char* name = present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                             : present_mode == VK_PRESENT_MODE_MAILBOX_KHR   ? "mailbox"
                             : present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "fifo_relaxed"
                                                                               : "fifo";
            std::printf("[gfxcoopa] present: %s, %u images (vsync %s)\n", name, actual_count,
                        vsync_ ? "on" : "off");
        }

        create_image_views();
    }

    /**
     * @brief Creates a VkImageView for each swapchain image.
     */
    void create_image_views() {
        image_views_.resize(images_.size());
        for (size_t i = 0; i < images_.size(); ++i) {
            VkImageViewCreateInfo view_info{};
            view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image                           = images_[i];
            view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format                          = image_format_;
            view_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
            view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.baseMipLevel   = 0;
            view_info.subresourceRange.levelCount     = 1;
            view_info.subresourceRange.baseArrayLayer = 0;
            view_info.subresourceRange.layerCount     = 1;

            GFX_VK_CHECK(vkCreateImageView(device_.handle(), &view_info, nullptr,
                                           &image_views_[i]));
        }
    }

    /**
     * @brief Destroys all image views and the swapchain.
     */
    void destroy() {
        for (VkImageView view : image_views_) {
            vkDestroyImageView(device_.handle(), view, nullptr);
        }
        image_views_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_.handle(), swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
    }

    // --- Format / mode / extent selection ---

    /**
     * @brief Selects the preferred surface format from the available list.
     *
     * Prefers B8G8R8A8_SRGB with SRGB_NONLINEAR color space, which gives
     * correct gamma-corrected rendering without a manual gamma pass.
     *
     * @param formats Available surface formats.
     * @return The chosen VkSurfaceFormatKHR.
     */
    static VkSurfaceFormatKHR choose_format(const std::vector<VkSurfaceFormatKHR>& formats) {
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return f;
            }
        }
        return formats[0]; // Fallback: first available.
    }

    /**
     * @brief Selects the preferred present mode.
     *
     * `vsync: false` means IMMEDIATE: frames go to the presentation engine as they finish,
     * which is the only mode that neither paces the frame rate nor blocks the thread that
     * presents. MAILBOX is the fallback where IMMEDIATE is unsupported -- it is tear-free but
     * still vblank-synced, so it caps at the refresh rate, and on a compositing X11 desktop its
     * `vkQueuePresentKHR` can block for several refreshes at a time (~60 ms here, once per
     * mouse press; measured on an RTX 4070, driver-side, with the GPU 3 ms into a 16 ms frame).
     * FIFO is vsync proper, guaranteed by the spec to exist, and shows the same blocking.
     *
     * `GFX_PRESENT_MODE=immediate|mailbox|fifo` forces one, for measuring what a given window
     * system does with each: a present that blocks for tens of milliseconds while
     * vkQueueSubmit returns instantly is the presentation engine, not the renderer (see
     * presentation::Renderer::last_present_ms()).
     *
     * @param modes Available present modes.
     * @return The chosen VkPresentModeKHR.
     */
    VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) const {
        const auto has = [&modes](VkPresentModeKHR m) {
            return std::find(modes.begin(), modes.end(), m) != modes.end();
        };
        const char* forced = std::getenv("GFX_PRESENT_MODE");
        const std::string want = forced != nullptr ? std::string(forced) : std::string();
        if (want == "fifo") return VK_PRESENT_MODE_FIFO_KHR;
        if (want == "immediate" && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        if (want == "mailbox" && has(VK_PRESENT_MODE_MAILBOX_KHR)) {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        if (!vsync_) {
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    /**
     * @brief Clamps the desired resolution to the surface's capability range.
     * @param caps    Surface capabilities.
     * @param width   Desired width in pixels.
     * @param height  Desired height in pixels.
     * @return The clamped VkExtent2D.
     */
    static VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps,
                                    uint32_t width, uint32_t height)
    {
        // When currentExtent is UINT32_MAX the surface lets us choose freely.
        if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return caps.currentExtent;
        }

        VkExtent2D extent = { width, height };
        extent.width  = std::clamp(extent.width,
                                   caps.minImageExtent.width,
                                   caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height,
                                   caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
        return extent;
    }

    Device&                  device_;                       /**< Owning logical device (not owned). */
    const Surface&           surface_;                      /**< Target window surface (not owned). */
    bool                     vsync_;                        /**< Vsync preference. */
    VkSwapchainKHR           swapchain_    = VK_NULL_HANDLE;/**< The Vulkan swapchain. */
    std::vector<VkImage>     images_;                       /**< Swapchain images (owned by the swapchain). */
    std::vector<VkImageView> image_views_;                  /**< One view per swapchain image. */
    VkFormat                 image_format_ = VK_FORMAT_UNDEFINED; /**< Selected swapchain image format. */
    VkExtent2D               extent_       = {0, 0};        /**< Current render resolution. */
};

} // namespace core
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_CORE_SWAPCHAIN_H
