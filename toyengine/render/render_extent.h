/**
 * @file render_extent.h
 * @brief Internal render resolution, integer-scale letterboxing and camera pixel-snap density.
 *
 * Header-only, dependency-free beyond glm, and exercised directly by toyengine_tests --
 * no Vulkan device is needed. Split out of toy_render_math.h, which still includes every
 * part, so an existing `#include <toyengine/render/toy_render_math.h>` keeps working.
 */

#ifndef TOYENGINE_RENDER_RENDER_EXTENT_H
#define TOYENGINE_RENDER_RENDER_EXTENT_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <toyengine/render/render_types.h>
#include <toyengine/render/toy_render_config.h>

namespace toy {
namespace render {

/**
 * @brief Derives the internal render target dimensions from config.
 * @param config Render configuration.
 * @param swapchain_w Current swapchain width, used by "divisor" mode.
 * @param swapchain_h Current swapchain height, used by "divisor" mode.
 * @return The internal (width, height), each clamped to a minimum of 1.
 */
inline RenderExtent compute_render_extent(const ToyRenderConfig& config,
                                          uint32_t swapchain_w, uint32_t swapchain_h) {
    RenderExtent extent;
    if (config.resolution_mode == "fill") {
        // Fixed vertical resolution, width following the display aspect (fill_aspect, set by
        // the Engine from the display region; 0 = the swapchain's).
        const float aspect = config.fill_aspect > 0.0f
            ? config.fill_aspect
            : static_cast<float>(std::max<uint32_t>(1, swapchain_w)) / static_cast<float>(std::max<uint32_t>(1, swapchain_h));
        extent.height = std::max<uint32_t>(1, config.render_height);
        extent.width  = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(extent.height * aspect)));
    } else if (config.resolution_mode == "divisor") {
        const uint32_t divisor = std::max<uint32_t>(1, config.scale_divisor);
        extent.width  = std::max<uint32_t>(1, swapchain_w / divisor);
        extent.height = std::max<uint32_t>(1, swapchain_h / divisor);
    } else {
        extent.width  = std::max<uint32_t>(1, config.render_width);
        extent.height = std::max<uint32_t>(1, config.render_height);
    }
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
 * @brief Computes a fractional, aspect-preserving best-fit destination rect.
 *
 * scale = min(sw/rw, sh/rh), kept fractional (no integer snapping) so the
 * render fills as much of the swapchain as the render aspect allows --
 * letterboxing only the one axis whose aspect doesn't match, rather than
 * every axis down to the nearest whole scale factor the way
 * compute_letterbox() does. Despite the historical name this preserves
 * aspect; it does not stretch to fill both axes.
 *
 * @param sw Swapchain width in pixels.
 * @param sh Swapchain height in pixels.
 * @param rw Low-resolution render width in pixels.
 * @param rh Low-resolution render height in pixels.
 * @return The centred destination rect, clamped to the swapchain extent so
 *   rounding can't leave a 1px gap on the axis that should fill exactly.
 */
inline LetterboxRect compute_fit(uint32_t sw, uint32_t sh, uint32_t rw, uint32_t rh) {
    float sx = static_cast<float>(sw) / static_cast<float>(std::max<uint32_t>(1, rw));
    float sy = static_cast<float>(sh) / static_cast<float>(std::max<uint32_t>(1, rh));
    float scale = std::min(sx, sy);
    uint32_t w = std::clamp(static_cast<uint32_t>(rw * scale + 0.5f), 1u, sw);
    uint32_t h = std::clamp(static_cast<uint32_t>(rh * scale + 0.5f), 1u, sh);
    LetterboxRect rect;
    rect.x     = static_cast<int32_t>((sw > w) ? (sw - w) / 2 : 0);
    rect.y     = static_cast<int32_t>((sh > h) ? (sh - h) / 2 : 0);
    rect.w     = w;
    rect.h     = h;
    rect.scale = scale;
    return rect;
}

/**
 * @brief Computes the final upscale's destination rect, per config.upscale_mode.
 *
 * "integer" (compute_letterbox()) snaps to a whole scale factor: crisp NxN
 * texel blocks, but can waste a large fraction of the window to letterbox
 * bars. Anything else (the default, "fit") fits the window as closely as the
 * render aspect allows (compute_fit()), letterboxing only the mismatched
 * axis -- at the cost of texel blocks alternating by a pixel at non-integer
 * scales, since sampling stays NEAREST either way.
 *
 * @param config Render configuration; only `upscale_mode` is read.
 * @param sw Swapchain width in pixels.
 * @param sh Swapchain height in pixels.
 * @param rw Low-resolution render width in pixels.
 * @param rh Low-resolution render height in pixels.
 * @return The centred destination rect for the chosen mode.
 */
inline LetterboxRect compute_display_rect(const ToyRenderConfig& config,
                                          uint32_t sw, uint32_t sh,
                                          uint32_t rw, uint32_t rh) {
    return (config.upscale_mode == "integer") ? compute_letterbox(sw, sh, rw, rh)
                                              : compute_fit(sw, sh, rw, rh);
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

#endif // TOYENGINE_RENDER_RENDER_EXTENT_H
