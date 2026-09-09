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
#include <limits>

#include <glm/glm.hpp>

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
    float    scale = 1.0f; /**< Swapchain pixels per low-res texel (fractional under "fit" mode). */
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
 * @param config Pixel render configuration; only `upscale_mode` is read.
 * @param sw Swapchain width in pixels.
 * @param sh Swapchain height in pixels.
 * @param rw Low-resolution render width in pixels.
 * @param rh Low-resolution render height in pixels.
 * @return The centred destination rect for the chosen mode.
 */
inline LetterboxRect compute_display_rect(const PixelRenderConfig& config,
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

/**
 * @struct SdfClipRect
 * @brief An SdfRenderer's world AABB projected into NDC by the active
 *        camera's view-proj matrix.
 */
struct SdfClipRect {
    glm::vec2 ndc_min{-1.0f}; /**< Clip-space (NDC) min, each component in [-1, 1]. */
    glm::vec2 ndc_max{1.0f};  /**< Clip-space (NDC) max, each component in [-1, 1]. */
    bool      visible = true; /**< False when the box projects to an empty (fully off-screen) rect. */
};

/**
 * @brief Projects a world AABB's 8 corners into NDC via `view_proj`, returning
 *        the enclosing rectangle -- the screen-space (pre-upscale) bounds an
 *        SdfRenderer's raymarch is scissored to.
 *
 * A corner behind (or exactly on) the near plane makes the projection
 * undefined for that corner (dividing by a near-zero or negative w), so any
 * such corner falls back the WHOLE box to the full-screen rect rather than
 * computing a partial (and potentially wildly wrong) bound from only the
 * corners that projected cleanly -- correctness over tightness for the rare
 * case of a camera inside, or very close to, the object's bounds.
 *
 * @param view_proj  Camera projection * view.
 * @param bounds_min World AABB min.
 * @param bounds_max World AABB max.
 * @return The enclosing NDC rect, or `visible = false` if every corner
 *         validly projected but the clamped rect is degenerate (fully
 *         off-screen) -- callers should skip the object entirely in that case.
 */
inline SdfClipRect compute_sdf_clip_rect(const glm::mat4& view_proj,
                                         const glm::vec3& bounds_min,
                                         const glm::vec3& bounds_max) {
    glm::vec2 ndc_min(std::numeric_limits<float>::max());
    glm::vec2 ndc_max(std::numeric_limits<float>::lowest());
    bool straddles_near = false;

    for (int c = 0; c < 8; ++c) {
        glm::vec3 corner((c & 1) ? bounds_max.x : bounds_min.x,
                         (c & 2) ? bounds_max.y : bounds_min.y,
                         (c & 4) ? bounds_max.z : bounds_min.z);
        glm::vec4 clip = view_proj * glm::vec4(corner, 1.0f);
        if (clip.w <= 1e-4f) {
            straddles_near = true;
            break;
        }
        glm::vec2 ndc = glm::vec2(clip.x, clip.y) / clip.w;
        ndc_min = glm::min(ndc_min, ndc);
        ndc_max = glm::max(ndc_max, ndc);
    }

    if (straddles_near) {
        return SdfClipRect{glm::vec2(-1.0f), glm::vec2(1.0f), true};
    }

    ndc_min = glm::clamp(ndc_min, glm::vec2(-1.0f), glm::vec2(1.0f));
    ndc_max = glm::clamp(ndc_max, glm::vec2(-1.0f), glm::vec2(1.0f));
    bool visible = ndc_min.x < ndc_max.x && ndc_min.y < ndc_max.y;
    return SdfClipRect{ndc_min, ndc_max, visible};
}

/** @brief An inclusive pixel rectangle, in the low-resolution render target's own pixel space. */
struct PixelRect {
    int32_t  x = 0;
    int32_t  y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

/**
 * @brief Converts an NDC rect (see compute_sdf_clip_rect()) to a pixel
 *        scissor rect in the low-resolution render target's own space.
 *
 * NDC.y follows this engine's usual OpenGL-style convention (+1 = top,
 * matching CameraComponent::get_projection_matrix()'s unflipped glm::ortho/
 * glm::perspective output), while framebuffer pixel space is Vulkan's
 * (y = 0 at the top). Every SDF pass that draws this rect shares the same
 * negative-viewport-height convention every other geometry target in this
 * engine already uses to reconcile the two (see gbuffer_target.h/
 * transparent_capture_target.h/transparent_pass.h's identical
 * `set_viewport(0, height, width, -height)` calls) -- that flip happens to
 * vertex positions during rasterization, but a dynamic scissor rect is
 * specified directly in framebuffer pixel space and is NOT affected by the
 * viewport transform, so this function performs the equivalent Y-flip by hand.
 *
 * @param ndc_min, ndc_max NDC rect (compute_sdf_clip_rect()'s output).
 * @param width, height    Render target size in pixels.
 * @return The scissor rect, clamped to [0, width] x [0, height].
 */
inline PixelRect sdf_clip_rect_to_pixels(const glm::vec2& ndc_min, const glm::vec2& ndc_max,
                                         uint32_t width, uint32_t height) {
    float px0 = (ndc_min.x * 0.5f + 0.5f) * static_cast<float>(width);
    float px1 = (ndc_max.x * 0.5f + 0.5f) * static_cast<float>(width);
    float py0 = (1.0f - (ndc_max.y * 0.5f + 0.5f)) * static_cast<float>(height); // top edge
    float py1 = (1.0f - (ndc_min.y * 0.5f + 0.5f)) * static_cast<float>(height); // bottom edge

    int32_t x0 = std::clamp(static_cast<int32_t>(std::floor(px0)), 0, static_cast<int32_t>(width));
    int32_t y0 = std::clamp(static_cast<int32_t>(std::floor(py0)), 0, static_cast<int32_t>(height));
    int32_t x1 = std::clamp(static_cast<int32_t>(std::ceil(px1)), 0, static_cast<int32_t>(width));
    int32_t y1 = std::clamp(static_cast<int32_t>(std::ceil(py1)), 0, static_cast<int32_t>(height));

    PixelRect rect;
    rect.x = x0;
    rect.y = y0;
    rect.w = (x1 > x0) ? static_cast<uint32_t>(x1 - x0) : 0;
    rect.h = (y1 > y0) ? static_cast<uint32_t>(y1 - y0) : 0;
    return rect;
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_MATH_H
