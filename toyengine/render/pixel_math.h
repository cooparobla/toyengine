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
#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
inline RenderExtent compute_render_extent(const PixelRenderConfig& config,
                                          uint32_t swapchain_w, uint32_t swapchain_h) {
    RenderExtent extent;
    if (config.resolution_mode == "divisor") {
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
 * @struct ShadowFitCamera
 * @brief The camera properties the directional-shadow fit reads -- nothing else about a
 *        camera matters to it.
 *
 * A plain struct rather than a CameraComponent so the fit stays pure glm and is testable
 * with no scene or Vulkan device (see toyengine_tests).
 */
struct ShadowFitCamera {
    glm::mat4 view{1.0f};            /**< World-to-view, i.e. inverse of the camera's world matrix. */
    bool  is_perspective   = true;
    float fov_degrees      = 45.0f;  /**< Vertical FOV; perspective only. */
    float orthographic_size = 5.0f;  /**< View-volume half-height in world units; orthographic only. */
    float near_clip        = 0.1f;
    float far_clip         = 1000.0f;
    float aspect           = 16.0f / 9.0f;
};

/**
 * @struct DirShadowFit
 * @brief compute_dir_shadow_fit()'s result: the light-space matrix to render the shadow
 *        map with, and the world size of one of its texels.
 */
struct DirShadowFit {
    glm::mat4 light_space_matrix{1.0f};
    float     texel_world = 0.0f; /**< World units per shadow-map texel in this frame's box. */
    float     depth_range_world = 0.0f; /**< World-unit span of the box's [0,1] light-space depth --
                                          what converts a stored-vs-receiver depth GAP back into
                                          metres (PCSS's penumbra estimate needs exactly that). */
};

/**
 * @brief The directional shadow's normal-offset bias, in WORLD units, for this frame's fit.
 *
 * Normal-offset shadows: before projecting a shading point into light space, push it off its
 * own surface along the geometric normal. Unlike a depth bias -- which shifts the comparison by
 * a constant and so over-biases head-on surfaces to fix grazing ones -- this scales with the
 * actual cause of the artifact, which is that one shadow-map texel covers more and more surface
 * as the surface tilts away from the light.
 *
 * The offset is therefore measured in TEXELS and converted to world units per frame, for exactly
 * the reason `shadow_softness` is (see PixelRenderConfig): the ortho box refits to the camera
 * every frame, so a world-space constant means a different number of texels in every scene and
 * at every fit. As a concrete scale: on assets/scenes/terrain_test one texel is ~0.076 world
 * units, so 0.05 world units would be ~0.66 texels -- less than the PCF disk's own 2.0-texel
 * reach, leaving every tap past 0.66 texels landing back on the surface's own depth.
 *
 * Which is why the PCF radius is part of the sum rather than something added on top by the
 * caller. A Vogel tap `r` texels away must still clear the surface, so the offset has to exceed
 * the disk's own reach before `extra_texels` buys any margin at all. With soft shadows off the
 * radius is 0 and the whole offset is `extra_texels`.
 *
 * `extra_texels` may be NEGATIVE, which dials the offset back inside the disk's own reach --
 * trading acne back for less peter-panning. Only the SUM is floored at zero: a negative total
 * would pull the sample into the surface, which is strictly worse than no bias at all.
 *
 * @param pcf_radius_texels The PCF disk's radius in shadow-map texels; 0 for a hard compare.
 * @param extra_texels      Margin beyond that reach, from PixelRenderConfig::shadow_normal_bias;
 *                          may be negative.
 * @param texel_world       World units per shadow-map texel, from DirShadowFit::texel_world.
 * @return The offset in world units, as the shader's `dir_shadow_params.w` wants it; never
 *         negative.
 */
inline float compute_shadow_normal_bias(float pcf_radius_texels, float extra_texels,
                                        float texel_world) {
    const float texels = std::max(pcf_radius_texels, 0.0f) + extra_texels;
    return std::max(texels * texel_world, 0.0f);
}

/**
 * @brief Fits an orthographic light-space box to a bounding sphere of the CAMERA's view
 *        frustum, clipped to `shadow_distance`, with texel-snapped centering.
 *
 * The technique behind Unity/Unreal's directional "shadow distance". Two properties matter
 * and both are deliberate:
 *
 *  - **It is a function of the camera alone.** No renderer, SDF or physics position is read,
 *    so nothing in the scene -- including an object that escaped the playable area and is in
 *    unbounded freefall -- can perturb or blow out the shadow frustum.
 *  - **It fits the frustum's bounding SPHERE, not its box.** A sphere's radius is invariant
 *    under camera rotation, so the box size is stable as the camera turns; snapping the
 *    centre to whole texels then removes the sub-texel crawl that remains under translation.
 *
 * The near plane sits a further `shadow_distance` behind the sphere on the towards-light
 * side, so a caster outside the visible sphere but between it and the light still shadows
 * into frame -- sized off that one config knob, never off any object's actual position.
 *
 * @param light_direction      Directional light's direction; normalized internally.
 * @param cam                  Active camera, or nullptr for the fixed fallback box.
 * @param shadow_distance      How far from the camera shadows are computed, in world units.
 * @param shadow_map_resolution Shadow map edge length in texels; drives the texel snap.
 * @return The light-space matrix and this frame's world-per-texel size.
 */
inline DirShadowFit compute_dir_shadow_fit(const glm::vec3& light_direction,
                                           const ShadowFitCamera* cam,
                                           float shadow_distance,
                                           uint32_t shadow_map_resolution) {
    const glm::vec3 light_dir = glm::normalize(light_direction);
    // Any up vector not parallel to the light works; swap axes near the poles of the
    // engine's Z-up convention so lookAt() never degenerates.
    const glm::vec3 up = (std::abs(light_dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                         : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 light_rot = glm::lookAt(glm::vec3(0.0f), light_dir, up);
    const float resolution = static_cast<float>(std::max(shadow_map_resolution, 1u));

    DirShadowFit fit;
    glm::mat4 light_proj;

    if (!cam) {
        // No camera yet: a fixed box, still purely a function of the light direction.
        const float ortho_extent = 15.0f;
        light_proj = glm::orthoRH_ZO(-ortho_extent, ortho_extent, -ortho_extent, ortho_extent,
                                     0.1f, 60.0f);
        fit.texel_world = (2.0f * ortho_extent) / resolution;
        fit.depth_range_world = 60.0f - 0.1f;
    } else {
        const glm::mat4 cam_to_world = glm::inverse(cam->view);
        const glm::vec3 cam_pos     = glm::vec3(cam_to_world[3]);
        const glm::vec3 cam_right   = glm::vec3(cam_to_world[0]);
        const glm::vec3 cam_up      = glm::vec3(cam_to_world[1]);
        const glm::vec3 cam_forward = -glm::vec3(cam_to_world[2]); // camera looks down local -Z

        const float near_d = cam->near_clip;
        const float far_d  = std::min(cam->far_clip, shadow_distance);

        // The 8 world-space frustum corners over [near_d, far_d]. Perspective corners scale
        // with distance; orthographic ones have the same half-extent at both planes.
        glm::vec3 corners[8];
        int idx = 0;
        for (float d : {near_d, far_d}) {
            const float half_h = cam->is_perspective
                ? d * std::tan(glm::radians(cam->fov_degrees) * 0.5f)
                : cam->orthographic_size;
            const float half_w = half_h * cam->aspect;
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sx = -1; sx <= 1; sx += 2) {
                    corners[idx++] = cam_pos + cam_forward * d
                                   + cam_right * (static_cast<float>(sx) * half_w)
                                   + cam_up    * (static_cast<float>(sy) * half_h);
                }
            }
        }

        glm::vec3 centroid(0.0f);
        for (const auto& c : corners) centroid += c;
        centroid /= 8.0f;
        float radius = 0.0f;
        for (const auto& c : corners) radius = std::max(radius, glm::length(c - centroid));

        const glm::vec3 center_ls = glm::vec3(light_rot * glm::vec4(centroid, 1.0f));

        // Quantize the extent too, so a slowly changing radius doesn't rescale the box (and
        // with it the texel grid the centre snaps to) on every single frame.
        const float pad = 1.0f;
        const float extent_step = 0.5f;
        const float extent = std::ceil((radius + pad) / extent_step) * extent_step;

        fit.texel_world = (2.0f * extent) / resolution;
        glm::vec2 center(center_ls.x, center_ls.y);
        center.x = std::floor(center.x / fit.texel_world) * fit.texel_world;
        center.y = std::floor(center.y / fit.texel_world) * fit.texel_world;

        const float far_plane  = -center_ls.z + radius + pad;
        float near_plane = -center_ls.z - radius - pad - shadow_distance;
        const float depth = std::max(far_plane - near_plane, 0.01f);
        fit.depth_range_world = depth;

        light_proj = glm::orthoRH_ZO(center.x - extent, center.x + extent,
                                     center.y - extent, center.y + extent,
                                     near_plane, near_plane + depth);
    }

    // No Vulkan Y-flip, deliberately. The shadow passes render with a POSITIVE-height
    // viewport (ShadowMapTarget::begin_directional_pass), so the plain RH_ZO matrix is
    // already the orientation the shader's `proj_coords.xy * 0.5 + 0.5` assumes --
    // the same convention ShadowMapTarget::get_spot_matrix() documents and uses.
    //
    // A flip applied as `light_proj[1][1] *= -1` would be wrong in any case: it negates
    // the ortho's Y SCALE without its Y TRANSLATION ([3][1]), which mirrors the box's
    // light-space centre about 0 instead of about itself. That is invisible near the
    // world origin -- where the centre is ~0 and the mirror is the identity -- and
    // pushes the box clean off the scene once the camera sits far out, as it does on
    // assets/scenes/terrain_test (centre ~-185, box half-extent ~79).
    fit.light_space_matrix = light_proj * light_rot;
    return fit;
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

/**
 * @brief View-space depth (metres along the camera's forward axis) of a world point.
 *
 * `view` follows this engine's usual convention -- glm::inverse(world_matrix), the
 * same one CameraComponent::get_view_matrix() and gfx's other view-space math use --
 * under which the camera looks down its own -Z, so a point in front of the eye has a
 * NEGATIVE view-space z. Negating gives a positive depth for anything in front,
 * matching gfx_linear_depth()'s convention (positive, growing from near to far) that
 * DofPass's dof_signed_coc() consumes as `view_depth_m`. A point behind the eye comes
 * back negative, which callers should treat as "no valid focus" rather than clamping
 * it positive (see resolve_dof_focus_()'s `depth > 0.0f` guard).
 *
 * @param view      Camera view matrix (world-to-view).
 * @param world_pos World-space point.
 * @return View-space depth in metres; negative if `world_pos` is behind the camera.
 */
inline float view_space_depth(const glm::mat4& view, const glm::vec3& world_pos) {
    return -(view * glm::vec4(world_pos, 1.0f)).z;
}

/**
 * @brief Half the extent of a transformed object-space AABB along the camera's forward axis.
 *
 * The standard "project an oriented box onto one axis" sum: for the model-view matrix's
 * third ROW (the one producing view-space z), the box's support along that axis is the sum
 * of the per-axis contributions taken in absolute value, which is exactly how far the box
 * reaches in front of and behind its own centre IN DEPTH. Taking `abs()` per term is also
 * what makes this independent of view_space_depth()'s sign convention -- a half-extent has
 * no direction, so the negation that turns view-space z into a positive depth is irrelevant.
 *
 * Written for DofPass::Params::focus_range: pairing this with the view depth of the SAME
 * box's centre gives the narrowest band that still holds a whole object sharp, which is the
 * one quantity the thin-lens CoC cannot supply (see resolve_dof_focus_()).
 *
 * @param view      Camera view matrix (world-to-view).
 * @param model     Object's world matrix.
 * @param local_min Object-space AABB minimum corner (e.g. Mesh::bounds_min()).
 * @param local_max Object-space AABB maximum corner (e.g. Mesh::bounds_max()).
 * @return Half the box's depth extent in metres; always >= 0.
 */
inline float view_depth_half_extent(const glm::mat4& view, const glm::mat4& model,
                                    const glm::vec3& local_min, const glm::vec3& local_max) {
    const glm::mat4 mv = view * model;
    const glm::vec3 he = (local_max - local_min) * 0.5f;
    return std::abs(mv[0].z * he.x) + std::abs(mv[1].z * he.y) + std::abs(mv[2].z * he.z);
}

/**
 * @brief World-space centre of an object-space AABB under a model matrix.
 *
 * The companion to view_depth_half_extent(): a mesh's transform ORIGIN is not generally its
 * centre (assets/scenes/pixel_demo/meshes/cube.000.yaml spans [0,1]^3, so its origin is a
 * corner), and focusing on the origin biases the focal plane off the subject by half its
 * depth.
 *
 * @param model     Object's world matrix.
 * @param local_min Object-space AABB minimum corner.
 * @param local_max Object-space AABB maximum corner.
 * @return The box centre in world space.
 */
inline glm::vec3 world_bounds_center(const glm::mat4& model,
                                     const glm::vec3& local_min, const glm::vec3& local_max) {
    return glm::vec3(model * glm::vec4((local_min + local_max) * 0.5f, 1.0f));
}

/**
 * @brief One frame of framerate-independent exponential easing toward `target`.
 *
 * Same `1 - exp(-rate * dt)` form as CameraController::update_orbit_()'s target
 * smoothing, so a lens focus-rack (see dof_focus_smoothing) reads as consistent with
 * the rest of this engine's follow/smoothing knobs: two half-steps at `dt` land at the
 * same place as one step at `2*dt`, which a naive linear lerp does not guarantee.
 *
 * @param current Current smoothed value.
 * @param target  Value being eased toward.
 * @param rate    Smoothing rate, 1/sec; <= 0 snaps `current` straight to `target`.
 * @param dt      Frame delta time, seconds.
 * @return The eased value for this frame.
 */
inline float exp_smooth_toward(float current, float target, float rate, float dt) {
    if (rate <= 0.0f) return target;
    float alpha = 1.0f - std::exp(-rate * dt);
    return current + (target - current) * alpha;
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_MATH_H
