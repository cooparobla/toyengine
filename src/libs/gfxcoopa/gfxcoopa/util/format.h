/**
 * @file format.h
 * @brief OpenGL-style Vulkan format mapping helpers.
 *
 * Provides utility functions that abstract over VkFormat selection in
 * the same spirit as OpenGL internal format tokens (GL_RGBA8, GL_DEPTH24_STENCIL8, etc.).
 */

#ifndef COOPA_GFX_UTIL_FORMAT_H
#define COOPA_GFX_UTIL_FORMAT_H

#include <volk/volk.h>
#include <stdexcept>
#include <cstdint>

namespace coopa {
namespace gfx {
namespace util {

/**
 * @brief Selects a VkFormat matching the given channel count and sRGB flag.
 *
 * Analogous to passing GL_RGBA8 or GL_SRGB8_ALPHA8 to glTexImage2D.
 * Channels must be 1, 2, or 4.
 *
 * @param channels Number of color channels (1=R, 2=RG, 4=RGBA).
 * @param srgb If true, returns an sRGB gamma-corrected format.
 * @return The corresponding VkFormat.
 * @throws std::invalid_argument for unsupported channel counts.
 */
inline VkFormat format_from_channels(int channels, bool srgb = false) {
    switch (channels) {
        case 1: return srgb ? VK_FORMAT_R8_SRGB       : VK_FORMAT_R8_UNORM;
        case 2: return srgb ? VK_FORMAT_R8G8_SRGB     : VK_FORMAT_R8G8_UNORM;
        case 4: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        default:
            throw std::invalid_argument(
                "[gfxcoopa] format_from_channels: unsupported channel count " +
                std::to_string(channels));
    }
}

/**
 * @brief Returns the byte size of a single texel for a given VkFormat.
 *
 * Covers common uncompressed formats. Returns 0 for unknown or
 * block-compressed formats.
 *
 * @param format The VkFormat to query.
 * @return Byte size per texel, or 0 if unknown.
 */
inline uint32_t format_byte_size(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:            return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SRGB:          return 2;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SRGB:        return 3;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:      return 4;
        case VK_FORMAT_R16_SFLOAT:         return 2;
        case VK_FORMAT_R16G16_SFLOAT:      return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:return 8;
        case VK_FORMAT_R32_SFLOAT:         return 4;
        case VK_FORMAT_R32G32_SFLOAT:      return 8;
        case VK_FORMAT_R32G32B32_SFLOAT:   return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;
        case VK_FORMAT_D16_UNORM:          return 2;
        case VK_FORMAT_D32_SFLOAT:         return 4;
        case VK_FORMAT_D24_UNORM_S8_UINT:  return 4;
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return 5; // 4 depth + 1 stencil (packed)
        default:                           return 0;
    }
}

/**
 * @brief Returns true if the format is a depth or depth-stencil format.
 * @param format The VkFormat to test.
 * @return True if depth/depth-stencil, false otherwise.
 */
inline bool format_has_depth(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM          ||
           format == VK_FORMAT_D32_SFLOAT          ||
           format == VK_FORMAT_D24_UNORM_S8_UINT   ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT  ||
           format == VK_FORMAT_X8_D24_UNORM_PACK32;
}

/**
 * @brief Returns true if the format has a stencil component.
 * @param format The VkFormat to test.
 * @return True if format contains a stencil channel.
 */
inline bool format_has_stencil(VkFormat format) {
    return format == VK_FORMAT_S8_UINT             ||
           format == VK_FORMAT_D24_UNORM_S8_UINT   ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

} // namespace util
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_UTIL_FORMAT_H
