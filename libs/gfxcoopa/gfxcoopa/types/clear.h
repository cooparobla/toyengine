/**
 * @file clear.h
 * @brief Small plain-data structs for extents, clear values, and image
 * subregions — gfxcoopa's replacements for VkExtent2D/VkClearColorValue/
 * VkOffset2D-shaped Vulkan structs in public signatures.
 */

#ifndef COOPA_GFX_TYPES_CLEAR_H
#define COOPA_GFX_TYPES_CLEAR_H

#include <cstdint>

namespace coopa {
namespace gfx {

/// @brief A 2D pixel extent (swapchain/render target size).
struct Extent2D {
    uint32_t width = 0;
    uint32_t height = 0;

    friend constexpr bool operator==(Extent2D, Extent2D) = default;
};

/// @brief An RGBA clear color in [0, 1] linear range (or raw texel bits for
/// integer targets, matching the prior VkClearColorValue::float32 usage —
/// no caller in this codebase clears an integer target).
struct ClearColor {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

/// @brief Depth/stencil clear values, paired with ClearColor when a render
/// pass has a depth attachment.
struct ClearDepthStencil {
    float    depth = 1.0f;
    uint32_t stencil = 0;
};

/// @brief The full set of clear values a render pass's begin needs — one
/// color, and depth/stencil only used if the pass has a depth attachment.
struct ClearValues {
    ClearColor         color;
    ClearDepthStencil  depth_stencil;
};

/// @brief A rectangular pixel region within an image (offset + extent),
/// used by CommandBuffer::blit() and CommandBuffer::set_scissor()-adjacent
/// copy/blit calls.
struct ImageRegion {
    int32_t  x = 0, y = 0;
    uint32_t width = 0, height = 0;
    uint32_t mip = 0;
    uint32_t layer = 0;
};

} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_TYPES_CLEAR_H
