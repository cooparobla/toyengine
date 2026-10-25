/**
 * @file sdf_clip.h
 * @brief Screen-space clip and scissor rects for SDF draws.
 *
 * Header-only, dependency-free beyond glm, and exercised directly by toyengine_tests --
 * no Vulkan device is needed. Split out of toy_render_math.h, which still includes every
 * part, so an existing `#include <toyengine/render/toy_render_math.h>` keeps working.
 */

#ifndef TOYENGINE_RENDER_SDF_CLIP_H
#define TOYENGINE_RENDER_SDF_CLIP_H

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace toy {
namespace render {

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

/** @brief An inclusive pixel rectangle, in the internal render target's own pixel space. */
struct PixelRect {
    int32_t  x = 0;
    int32_t  y = 0;
    uint32_t w = 0;
    uint32_t h = 0;
};

/**
 * @brief Converts an NDC rect (see compute_sdf_clip_rect()) to a pixel
 *        scissor rect in the internal render target's own space.
 *
 * NDC.y follows this engine's usual OpenGL-style convention (+1 = top,
 * matching CameraComponent::get_projection_matrix()'s unflipped glm::ortho/
 * glm::perspective output), while framebuffer pixel space is Vulkan's
 * (y = 0 at the top). Every SDF pass that draws this rect shares the same
 * negative-viewport-height convention every other geometry target in this
 * engine already uses to reconcile the two (see gbuffer_target.h/
 * transparent_pass.h's identical
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

#endif // TOYENGINE_RENDER_SDF_CLIP_H
