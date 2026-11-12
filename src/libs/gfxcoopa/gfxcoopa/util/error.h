/**
 * @file error.h
 * @brief Vulkan result checking utilities and error helpers for gfxcoopa.
 *
 * Provides a human-readable VkResult -> string mapping and a macro
 * GFX_VK_CHECK that wraps any vkXxx call, throwing std::runtime_error
 * on failure with context information.
 */

#ifndef COOPA_GFX_UTIL_ERROR_H
#define COOPA_GFX_UTIL_ERROR_H

#include <volk/volk.h>
#include <string>
#include <stdexcept>

namespace coopa {
namespace gfx {
namespace util {

/**
 * @brief Converts a VkResult code to a human-readable string.
 *
 * Covers all standard Vulkan result codes. Unknown values are returned
 * as a hex string for debugging.
 *
 * @param result The VkResult value to convert.
 * @return A string describing the result code.
 */
inline std::string vk_result_string(VkResult result) {
    switch (result) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN:                  return "VK_ERROR_UNKNOWN";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:    return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV:        return "VK_ERROR_INVALID_SHADER_NV";
        default:
            return "VK_RESULT_UNKNOWN(" + std::to_string(static_cast<int>(result)) + ")";
    }
}

/**
 * @brief Checks a VkResult and throws std::runtime_error on failure.
 *
 * @param result The VkResult returned by a Vulkan call.
 * @param context A descriptive string identifying the call site.
 * @throws std::runtime_error if result is not VK_SUCCESS.
 */
inline void vk_check(VkResult result, const std::string& context) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error("[gfxcoopa] Vulkan error in " + context +
                                 ": " + vk_result_string(result));
    }
}

} // namespace util
} // namespace gfx
} // namespace coopa

/**
 * @brief Wraps a Vulkan call and throws std::runtime_error on failure.
 *
 * Usage: GFX_VK_CHECK(vkCreateDevice(...));
 */
#define GFX_VK_CHECK(expr) \
    ::coopa::gfx::util::vk_check((expr), #expr)

#endif // COOPA_GFX_UTIL_ERROR_H
