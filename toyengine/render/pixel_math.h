/**
 * @file pixel_math.h
 * @brief Pure-CPU math for the pixel-art pipeline: internal resolution
 *        derivation, integer-scale letterboxing, and camera pixel-snap density.
 *
 * Everything here is header-only, dependency-free (besides glm/cstdint), and
 * exercised directly by toyengine_tests -- no Vulkan device is needed to
 * validate the letterbox and snapping arithmetic.
 */

#ifndef TOYENGINE_RENDER_PIXEL_MATH_H
#define TOYENGINE_RENDER_PIXEL_MATH_H

#include <algorithm>
#include <cstdint>

#include <toyengine/render/pixel_render_config.h>

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
    uint32_t scale = 1;   /**< Integer swapchain pixels per low-res texel. */
};

/**
 * @struct RenderExtent
 * @brief A low-resolution render target's (width, height) in pixels.
 */
struct RenderExtent {
    uint32_t width  = 480;
    uint32_t height = 270;
};

/**
 * @brief Derives the low-resolution render target dimensions from config.
 * @param config Pixel render configuration.
 * @param swapchain_w Current swapchain width, used by "divisor" mode.
 * @param swapchain_h Current swapchain height, used by "divisor" mode.
 * @return Low-resolution (width, height), each clamped to a minimum of 1.
 */
inline void compute_render_resolution(const PixelRenderConfig& config,
                                      uint32_t swapchain_w, uint32_t swapchain_h,
                                      uint32_t& out_w, uint32_t& out_h) {
    if (config.resolution_mode == "divisor") {
        uint32_t divisor = std::max<uint32_t>(1, config.scale_divisor);
        out_w = std::max<uint32_t>(1, swapchain_w / divisor);
        out_h = std::max<uint32_t>(1, swapchain_h / divisor);
    } else {
        out_w = std::max<uint32_t>(1, config.render_width);
        out_h = std::max<uint32_t>(1, config.render_height);
    }
}

/**
 * @brief Value-returning convenience wrapper around compute_render_resolution(),
 *        for use in a member initializer list where out-parameters are awkward.
 */
inline RenderExtent compute_render_extent(const PixelRenderConfig& config,
                                          uint32_t swapchain_w, uint32_t swapchain_h) {
    RenderExtent extent;
    compute_render_resolution(config, swapchain_w, swapchain_h, extent.width, extent.height);
    return extent;
}

/**
 * @brief Computes the centred, integer-scaled letterbox rect for upscaling.
 *
 * scale = floor(min(sw/rw, sh/rh)), clamped to a minimum of 1 so a window
 * smaller than the render resolution crops instead of producing a
 * zero-sized viewport. Every low-res texel maps to an exact scale x scale
 * block of swapchain pixels -- the defining trait of the integer upscale mode.
 *
 * @param sw Swapchain width in pixels.
 * @param sh Swapchain height in pixels.
 * @param rw Low-resolution render width in pixels.
 * @param rh Low-resolution render height in pixels.
 * @return The centred destination rect and the integer scale factor used.
 */
inline LetterboxRect compute_letterbox(uint32_t sw, uint32_t sh, uint32_t rw, uint32_t rh) {
    uint32_t scale = std::max<uint32_t>(1, std::min(sw / std::max<uint32_t>(1, rw),
                                                     sh / std::max<uint32_t>(1, rh)));
    uint32_t w = rw * scale;
    uint32_t h = rh * scale;
    LetterboxRect rect;
    rect.x     = static_cast<int32_t>((sw > w) ? (sw - w) / 2 : 0);
    rect.y     = static_cast<int32_t>((sh > h) ? (sh - h) / 2 : 0);
    rect.w     = w;
    rect.h     = h;
    rect.scale = scale;
    return rect;
}

/**
 * @brief Computes a fractional best-fit destination rect (no integer snapping).
 * @param sw Swapchain width in pixels.
 * @param sh Swapchain height in pixels.
 * @param rw Low-resolution render width in pixels.
 * @param rh Low-resolution render height in pixels.
 * @return The centred destination rect; `scale` is rounded for informational display only.
 */
inline LetterboxRect compute_stretch_fit(uint32_t sw, uint32_t sh, uint32_t rw, uint32_t rh) {
    float sx = static_cast<float>(sw) / static_cast<float>(std::max<uint32_t>(1, rw));
    float sy = static_cast<float>(sh) / static_cast<float>(std::max<uint32_t>(1, rh));
    float scale = std::min(sx, sy);
    uint32_t w = static_cast<uint32_t>(rw * scale);
    uint32_t h = static_cast<uint32_t>(rh * scale);
    LetterboxRect rect;
    rect.x     = static_cast<int32_t>((sw > w) ? (sw - w) / 2 : 0);
    rect.y     = static_cast<int32_t>((sh > h) ? (sh - h) / 2 : 0);
    rect.w     = w;
    rect.h     = h;
    rect.scale = static_cast<uint32_t>(scale + 0.5f);
    return rect;
}

/**
 * @brief Computes the world-space units per render-pixel used for camera snapping.
 *
 * Only meaningful under an orthographic projection, where world-to-pixel
 * scale is constant across the frame. Under perspective it varies with
 * depth, so snapping is intentionally disabled (returns 0) -- callers should
 * warn once when camera_pixel_snap is requested on a perspective camera.
 *
 * @param is_orthographic  Whether the active camera is orthographic.
 * @param orthographic_size Half-height of the orthographic view volume, in world units.
 * @param render_height    Low-resolution render height in pixels.
 * @return World units per render pixel, or 0 to disable snapping.
 */
inline float compute_pixel_density(bool is_orthographic, float orthographic_size, uint32_t render_height) {
    if (!is_orthographic || render_height == 0) {
        return 0.0f;
    }
    return (2.0f * orthographic_size) / static_cast<float>(render_height);
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_MATH_H
