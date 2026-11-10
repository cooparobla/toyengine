/**
 * @file debug_messenger.h
 * @brief RAII wrapper around VkDebugUtilsMessengerEXT for Vulkan validation output.
 *
 * Installs a validation layer callback that routes messages through
 * coopa::debug::Logger, mapping Vulkan severity to info/warn/error levels.
 * Created automatically by Instance when validation is enabled.
 */

#ifndef COOPA_GFX_UTIL_DEBUG_MESSENGER_H
#define COOPA_GFX_UTIL_DEBUG_MESSENGER_H

#include <volk/volk.h>
#include <string>
#include <iostream>

#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace util {

/**
 * @class DebugMessenger
 * @brief RAII wrapper around VkDebugUtilsMessengerEXT.
 *
 * Registers a Vulkan debug callback on construction and destroys it on
 * destruction. Routes validation layer output to stderr with severity
 * prefix tags matching the libcoopa Logger format.
 */
class DebugMessenger {
public:
    /**
     * @brief Creates and registers the debug messenger on the given instance.
     * @param instance The VkInstance to attach the messenger to.
     */
    explicit DebugMessenger(VkInstance instance) : instance_(instance) {
        VkDebugUtilsMessengerCreateInfoEXT create_info = make_create_info();
        GFX_VK_CHECK(vkCreateDebugUtilsMessengerEXT(instance_, &create_info, nullptr, &messenger_));
    }

    /**
     * @brief Destroys the debug messenger.
     */
    ~DebugMessenger() {
        if (messenger_ != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
    }

    /// @brief Non-copyable.
    DebugMessenger(const DebugMessenger&) = delete;
    /// @brief Non-copyable.
    DebugMessenger& operator=(const DebugMessenger&) = delete;

    /**
     * @brief Returns a VkDebugUtilsMessengerCreateInfoEXT pre-filled with
     *        gfxcoopa's callback. Used by Instance during instance creation
     *        so validation is active from the very first Vulkan call.
     * @return A populated create info struct.
     */
    static VkDebugUtilsMessengerCreateInfoEXT make_create_info() {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT     |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT  |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = debug_callback;
        info.pUserData       = nullptr;
        return info;
    }

private:
    /**
     * @brief Vulkan validation layer callback.
     *
     * Maps VkDebugUtilsMessageSeverityFlagBitsEXT to stderr output with
     * INFO/WARN/ERROR tags consistent with libcoopa Logger format.
     *
     * @param severity Message severity bitmask.
     * @param type Message type bitmask.
     * @param data Message data containing the human-readable string.
     * @param user_data Unused user-defined pointer.
     * @return VK_FALSE (must not return VK_TRUE per spec).
     */
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT  severity,
        VkDebugUtilsMessageTypeFlagsEXT         type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void*                                   user_data)
    {
        (void)type;
        (void)user_data;

        const char* tag = "INFO";
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   tag = "ERROR";
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) tag = "WARN";

        std::cerr << "[" << tag << "]::[gfx::validation] " << data->pMessage << "\n";
        return VK_FALSE;
    }

    VkInstance               instance_   = VK_NULL_HANDLE; /**< The owning Vulkan instance. */
    VkDebugUtilsMessengerEXT messenger_  = VK_NULL_HANDLE; /**< The underlying messenger handle. */
};

} // namespace util
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_UTIL_DEBUG_MESSENGER_H
