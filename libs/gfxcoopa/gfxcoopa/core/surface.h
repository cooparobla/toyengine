/**
 * @file surface.h
 * @brief RAII wrapper around VkSurfaceKHR for GLFW-based window surfaces.
 *
 * Creates a platform-agnostic Vulkan surface from a GLFW window and exposes
 * helpers for querying swapchain support details.
 */

#ifndef COOPA_GFX_CORE_SURFACE_H
#define COOPA_GFX_CORE_SURFACE_H

#include <volk/volk.h>
#include <vector>
#include <stdexcept>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <gfxcoopa/core/instance.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/presentation/window.h>

namespace coopa {
namespace gfx {
namespace core {

/**
 * @struct SwapchainSupportDetails
 * @brief Aggregates the three pieces of information needed to create a swapchain.
 */
struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR        capabilities;   /**< Min/max image count, extents, transforms. */
    std::vector<VkSurfaceFormatKHR> formats;        /**< Available pixel format + color space pairs. */
    std::vector<VkPresentModeKHR>   present_modes;  /**< Available presentation modes (FIFO, MAILBOX, etc.). */
};

/**
 * @class Surface
 * @brief RAII wrapper around VkSurfaceKHR created from a GLFW window.
 *
 * The surface abstracts the platform-specific window system connection.
 * It must outlive any Swapchain or Device that references it.
 */
class Surface {
public:
    /**
     * @brief Creates a Vulkan surface from a GLFW window.
     *
     * Delegates to glfwCreateWindowSurface() which selects the correct
     * platform backend (XCB, Wayland, Win32, etc.) automatically.
     *
     * @param instance The Vulkan instance.
     * @param window Raw GLFWwindow pointer (from Window::handle()).
     * @throws std::runtime_error if surface creation fails.
     */
    Surface(const Instance& instance, GLFWwindow* window)
        : instance_(instance.handle())
    {
        GFX_VK_CHECK(glfwCreateWindowSurface(instance_, window, nullptr, &surface_));
    }

    /**
     * @brief Creates a Vulkan surface from a sealed presentation::Window.
     *
     * The last standing GLFW->Vulkan coupling a consumer could otherwise be
     * forced to name directly (Window::handle() returning GLFWwindow*) --
     * this overload lets app::Context (and any other caller) construct a
     * Surface from a Window without ever naming GLFWwindow* itself.
     *
     * @param instance The Vulkan instance.
     * @param window The window to create a surface for.
     * @throws std::runtime_error if surface creation fails.
     */
    Surface(const Instance& instance, const presentation::Window& window)
        : Surface(instance, window.handle())
    {}

    /**
     * @brief Destroys the VkSurfaceKHR.
     */
    ~Surface() {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
    }

    /// @brief Non-copyable.
    Surface(const Surface&) = delete;
    /// @brief Non-copyable.
    Surface& operator=(const Surface&) = delete;

    /**
     * @brief Returns the underlying VkSurfaceKHR handle.
     * @return Raw VkSurfaceKHR.
     */
    VkSurfaceKHR handle() const { return surface_; }

    /**
     * @brief Queries swapchain support details for a given physical device.
     *
     * Retrieves surface capabilities, supported formats, and supported
     * present modes. Used by Device to verify compatibility and by
     * Swapchain to select optimal settings.
     *
     * @param physical_device The physical device to query.
     * @return A SwapchainSupportDetails struct.
     */
    SwapchainSupportDetails query_support(VkPhysicalDevice physical_device) const {
        SwapchainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface_,
                                                  &details.capabilities);

        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_,
                                             &format_count, nullptr);
        if (format_count > 0) {
            details.formats.resize(format_count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface_,
                                                 &format_count, details.formats.data());
        }

        uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_,
                                                   &mode_count, nullptr);
        if (mode_count > 0) {
            details.present_modes.resize(mode_count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface_,
                                                       &mode_count,
                                                       details.present_modes.data());
        }

        return details;
    }

private:
    VkInstance   instance_ = VK_NULL_HANDLE; /**< Owning instance (not owned). */
    VkSurfaceKHR surface_  = VK_NULL_HANDLE; /**< The platform window surface. */
};

} // namespace core
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_CORE_SURFACE_H
