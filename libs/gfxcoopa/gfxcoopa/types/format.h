/**
 * @file format.h
 * @brief gfxcoopa-owned pixel format enum and helpers.
 *
 * Supersedes util/format.h, which operated on VkFormat directly. Format is
 * deliberately narrow (~20 values) rather than a mirror of VkFormat's ~300 —
 * values are added here on demand as a real caller needs them.
 */

#ifndef COOPA_GFX_TYPES_FORMAT_H
#define COOPA_GFX_TYPES_FORMAT_H

#include <cstdint>
#include <stdexcept>
#include <string>

namespace coopa {
namespace gfx {

/**
 * @enum Format
 * @brief A pixel/vertex-attribute format, gfxcoopa's replacement for VkFormat
 * in every public signature.
 */
enum class Format : uint16_t {
    Undefined = 0,

    R8_Unorm,
    R8_Srgb,
    RG8_Unorm,
    RG8_Srgb,
    RGBA8_Unorm,
    RGBA8_Srgb,
    BGRA8_Unorm,
    BGRA8_Srgb,

    R16_Sfloat,
    RG16_Sfloat,
    RGBA16_Sfloat,
    A2B10G10R10_Unorm,

    R32_Sfloat,
    RG32_Sfloat,
    RGB32_Sfloat,
    RGBA32_Sfloat,
    R32_Uint,
    RG32_Uint,
    RGBA32_Uint,

    D16_Unorm,
    D32_Sfloat,
    D24_Unorm_S8_Uint,
    D32_Sfloat_S8_Uint,
};

/**
 * @brief Selects a Format matching the given channel count and sRGB flag.
 *
 * Analogous to passing GL_RGBA8 or GL_SRGB8_ALPHA8 to glTexImage2D.
 * Channels must be 1, 2, or 4.
 *
 * @param channels Number of color channels (1=R, 2=RG, 4=RGBA).
 * @param srgb If true, returns an sRGB gamma-corrected format.
 * @return The corresponding Format.
 * @throws std::invalid_argument for unsupported channel counts.
 */
inline Format format_from_channels(int channels, bool srgb = false) {
    switch (channels) {
        case 1: return srgb ? Format::R8_Srgb    : Format::R8_Unorm;
        case 2: return srgb ? Format::RG8_Srgb   : Format::RG8_Unorm;
        case 4: return srgb ? Format::RGBA8_Srgb : Format::RGBA8_Unorm;
        default:
            throw std::invalid_argument(
                "[gfxcoopa] format_from_channels: unsupported channel count " +
                std::to_string(channels));
    }
}

/**
 * @brief Returns the byte size of a single texel/element for a given Format.
 * @param format The Format to query.
 * @return Byte size per texel, or 0 for the block-compressed/unsupported case.
 */
inline uint32_t format_byte_size(Format format) {
    switch (format) {
        case Format::R8_Unorm:
        case Format::R8_Srgb:              return 1;
        case Format::RG8_Unorm:
        case Format::RG8_Srgb:             return 2;
        case Format::RGBA8_Unorm:
        case Format::RGBA8_Srgb:
        case Format::BGRA8_Unorm:
        case Format::BGRA8_Srgb:
        case Format::A2B10G10R10_Unorm:    return 4;
        case Format::R16_Sfloat:           return 2;
        case Format::RG16_Sfloat:          return 4;
        case Format::RGBA16_Sfloat:        return 8;
        case Format::R32_Sfloat:
        case Format::R32_Uint:             return 4;
        case Format::RG32_Sfloat:
        case Format::RG32_Uint:            return 8;
        case Format::RGB32_Sfloat:         return 12;
        case Format::RGBA32_Sfloat:
        case Format::RGBA32_Uint:          return 16;
        case Format::D16_Unorm:            return 2;
        case Format::D32_Sfloat:           return 4;
        case Format::D24_Unorm_S8_Uint:    return 4;
        case Format::D32_Sfloat_S8_Uint:   return 5; // 4 depth + 1 stencil (packed)
        default:                           return 0;
    }
}

/// @brief True if `format` has a depth component (with or without stencil).
inline bool is_depth(Format format) {
    return format == Format::D16_Unorm          ||
           format == Format::D32_Sfloat          ||
           format == Format::D24_Unorm_S8_Uint   ||
           format == Format::D32_Sfloat_S8_Uint;
}

/// @brief True if `format` has a stencil component.
inline bool is_stencil(Format format) {
    return format == Format::D24_Unorm_S8_Uint ||
           format == Format::D32_Sfloat_S8_Uint;
}

/// @brief True if `format` is sRGB-encoded (gamma-corrected).
inline bool is_srgb(Format format) {
    return format == Format::R8_Srgb   ||
           format == Format::RG8_Srgb  ||
           format == Format::RGBA8_Srgb ||
           format == Format::BGRA8_Srgb;
}

/// @brief True if `format`'s channel order is BGRA rather than RGBA —
/// e.g. some platforms' native swapchain format. Callers writing pixels
/// out to a channel-ordered buffer (PNG, etc.) must swizzle when true.
inline bool is_bgra(Format format) {
    return format == Format::BGRA8_Unorm || format == Format::BGRA8_Srgb;
}

} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_TYPES_FORMAT_H
