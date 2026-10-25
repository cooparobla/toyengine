/**
 * @file render_types.h
 * @brief The two plain rects the render target and its display are described by.
 *
 * Header-only, dependency-free beyond glm, and exercised directly by toyengine_tests --
 * no Vulkan device is needed. Split out of toy_render_math.h, which still includes every
 * part, so an existing `#include <toyengine/render/toy_render_math.h>` keeps working.
 */

#ifndef TOYENGINE_RENDER_RENDER_TYPES_H
#define TOYENGINE_RENDER_RENDER_TYPES_H

#include <cstdint>

namespace toy {
namespace render {

/**
 * @struct LetterboxRect
 * @brief A centred, integer-scaled viewport rect within the swapchain.
 */
struct LetterboxRect {
    int32_t  x = 0;      /**< Left edge in swapchain pixels. */
    int32_t  y = 0;      /**< Top edge in swapchain pixels. */
    uint32_t w = 0;       /**< Width in swapchain pixels. */
    uint32_t h = 0;       /**< Height in swapchain pixels. */
    float    scale = 1.0f; /**< Swapchain pixels per low-res texel (fractional under "fit" mode). */
};

/**
 * @struct RenderExtent
 * @brief The internal render target's (width, height) in pixels.
 */
struct RenderExtent {
    uint32_t width  = 1920;
    uint32_t height = 1080;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_RENDER_TYPES_H
