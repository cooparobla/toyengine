/**
 * @file shadow_fit.h
 * @brief Directional-shadow frustum fitting: cascade splits, clipmap radii, the atlas tiling.
 *
 * Header-only, dependency-free beyond glm, and exercised directly by toyengine_tests --
 * no Vulkan device is needed. Split out of toy_render_math.h, which still includes every
 * part, so an existing `#include <toyengine/render/toy_render_math.h>` keeps working.
 */

#ifndef TOYENGINE_RENDER_SHADOW_FIT_H
#define TOYENGINE_RENDER_SHADOW_FIT_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace toy {
namespace render {

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
 * the reason `shadow_softness` is (see ToyRenderConfig): the ortho box refits to the camera
 * every frame, so a world-space constant means a different number of texels in every scene and
 * at every fit. As a concrete scale: on assets/scenes/terrain/terrain_demo one texel is ~0.076 world
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
 * @param extra_texels      Margin beyond that reach, from ToyRenderConfig::shadow_normal_bias;
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
 * @brief The view-space depth boundaries of a cascaded shadow map's slices.
 *
 * Entry `i` is the FAR edge of cascade `i`; the near edge of cascade 0 is `near_clip` and
 * the near edge of cascade `i > 0` is `splits[i - 1]`. Entries past `count - 1` repeat the
 * last boundary, so a caller that always reads four of them never sees an uninitialised one.
 */
struct CascadeSplits {
    float distance[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

/**
 * @brief Practical split scheme (Zhang et al.): a `lambda` blend of the logarithmic and
 *        uniform cascade distributions.
 *
 * The logarithmic distribution is what equalizes world-per-texel across cascades -- it is
 * the correct answer for perspective projection -- but on its own it crushes the first
 * cascade down to centimetres around a small `near_clip`, spending a whole tile on geometry
 * that is mostly inside the camera. The uniform distribution has the opposite failure, a
 * first cascade so large it barely improves on one map. Blending them is the standard fix;
 * `lambda` = 1 is fully logarithmic, 0 fully uniform.
 *
 * @param near_clip       Camera near plane, in world units; floored at 0.01 so the log term
 *                        stays finite.
 * @param shadow_distance Far edge of the last cascade -- the whole shadowed range.
 * @param count           Live cascade count; clamped to [1, 8].
 * @param lambda          Log/uniform blend in [0, 1].
 * @return The four boundaries, the last of which is always `shadow_distance`.
 */
inline CascadeSplits compute_cascade_splits(float near_clip, float shadow_distance,
                                            uint32_t count, float lambda) {
    const int   n    = static_cast<int>(std::clamp<uint32_t>(count, 1u, 8u));
    const float near_d = std::max(near_clip, 0.01f);
    const float far_d  = std::max(shadow_distance, near_d + 0.01f);
    const float ratio  = far_d / near_d;
    const float lerp_t = std::clamp(lambda, 0.0f, 1.0f);

    CascadeSplits splits;
    for (int i = 0; i < n; ++i) {
        const float p       = static_cast<float>(i + 1) / static_cast<float>(n);
        const float log_d   = near_d * std::pow(ratio, p);
        const float uni_d   = near_d + (far_d - near_d) * p;
        splits.distance[i]  = lerp_t * log_d + (1.0f - lerp_t) * uni_d;
    }
    // The last cascade must reach exactly shadow_distance -- the blend above lands there
    // analytically, but only up to float error, and this is the boundary past which
    // calc_dir_shadow() stops shadowing at all.
    splits.distance[n - 1] = far_d;
    for (int i = n; i < 8; ++i) splits.distance[i] = far_d;
    return splits;
}

/**
 * @brief Radii of the `"focus"` fit's nested cascades: geometric from `inner` to `outer`.
 *
 * Each cascade covers a sphere around the focus point, so equal growth ratios give every
 * cascade the same texel-to-radius ratio -- the focus-fit counterpart of a logarithmic split.
 *
 * @param inner  Radius of the finest cascade (ToyRenderConfig::shadow_focus_radius).
 * @param outer  Radius of the coarsest (ToyRenderConfig::shadow_distance); raised to `inner`
 *               if smaller.
 * @param count  Cascades, 1..MAX; a single cascade takes `outer`.
 * @return One radius per cascade, finest first.
 */
inline std::vector<float> compute_focus_cascade_radii(float inner, float outer, uint32_t count) {
    std::vector<float> radii;
    const float r0 = std::max(inner, 0.01f);
    const float r1 = std::max(outer, r0);
    if (count <= 1) {
        radii.push_back(r1);
        return radii;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        radii.push_back(r0 * std::pow(r1 / r0, t));
    }
    return radii;
}

/**
 * @brief Quantizes a light direction to a grid `step_degrees` coarse, in azimuth and elevation.
 *
 * A moving sun (a day/night cycle) changes every level's light-space basis every frame, which
 * dirties every cached level every frame and makes static-depth caching worthless however well
 * the level centres are snapped. Snapping the direction the SHADOW FIT uses -- never the
 * direction the lighting shades with, which stays continuous -- turns that back into a step
 * every few seconds. The cost is that shadows rotate in visible increments; at a quarter of a
 * degree, on a 20-minute day, that is a step roughly every second and a shift far smaller than
 * the penumbra hides.
 *
 * `step_degrees <= 0` returns the direction unchanged (no snapping).
 */
inline glm::vec3 snap_light_direction(const glm::vec3& direction, float step_degrees) {
    const glm::vec3 d = glm::normalize(direction);
    if (!(step_degrees > 0.0f)) return d;
    const float step = glm::radians(step_degrees);
    // Z-up, matching the fit's own `up` choice: elevation off the XY plane, azimuth within it.
    float azimuth   = std::atan2(d.y, d.x);
    float elevation = std::asin(std::clamp(d.z, -1.0f, 1.0f));
    azimuth   = std::round(azimuth / step) * step;
    elevation = std::round(elevation / step) * step;
    const float ce = std::cos(elevation);
    return glm::vec3(ce * std::cos(azimuth), ce * std::sin(azimuth), std::sin(elevation));
}

/**
 * @brief Radii of the `"clipmap"` fit's concentric levels: geometric from `inner` outward.
 *
 * Unlike compute_focus_cascade_radii(), which spreads `count` radii between a fixed inner and
 * outer, a clipmap's reach is an OUTPUT: level `i` has radius `inner * ratio^i`, so the set
 * covers `inner * ratio^(count-1)` whatever that works out to. That is the point of the fit --
 * the shadowed range follows from the level budget rather than being clamped to a configured
 * distance, which is what lets it reach kilometres without starving the near levels.
 *
 * @param inner Radius of the finest level (ToyRenderConfig::shadow_clip_radius); floored at 0.01.
 * @param ratio Growth per level (ToyRenderConfig::shadow_clip_ratio); clamped to [1.3, 4].
 * @param count Levels, 1..MAX_DIR_SHADOW_LEVELS.
 * @return One radius per level, finest first.
 */
inline std::vector<float> compute_clipmap_level_radii(float inner, float ratio, uint32_t count) {
    const float r0 = std::max(inner, 0.01f);
    const float k  = std::clamp(ratio, 1.3f, 4.0f);
    std::vector<float> radii;
    radii.reserve(count);
    float r = r0;
    for (uint32_t i = 0; i < count; ++i) {
        radii.push_back(r);
        r *= k;
    }
    return radii;
}

/**
 * @brief Fits a texel-snapped orthographic light-space box around a world-space sphere.
 *
 * The shared core of every directional cascade fit. compute_dir_shadow_fit_slice() calls it
 * with a sphere bounding one slice of the camera frustum; the `"focus"` fit calls it with
 * spheres of fixed radius around the camera's focus point (see
 * ToyRenderConfig::shadow_fit). The extent is quantized to 0.5 m steps and the centre
 * snapped to whole texels, so a slowly changing radius does not rescale the texel grid every
 * frame and a moving centre does not crawl sub-texel.
 *
 * @param light_direction Directional light's direction; normalized internally.
 * @param centre          Sphere centre, world space.
 * @param radius          Sphere radius, world units.
 * @param caster_reach    Extra depth pulled in on the towards-light side, so a caster
 *                        outside the sphere but between it and the light still shadows into it.
 * @param tile_resolution Edge length of the cascade's atlas TILE in texels; drives the snap.
 * @param snap_step_texels Snap the centre to a grid this many texels coarse, and pad the extent
 *                         by the same amount so the box still covers `radius` while its snapped
 *                         centre lags. 1 (the default) is the finest snap and the tightest box,
 *                         and keeps every existing caller bit-identical. Larger steps make the
 *                         matrix PIECEWISE CONSTANT in the centre -- it changes only when the
 *                         centre crosses a step boundary -- which is what lets a level's
 *                         rendered depth be cached across frames (see DirShadowCache). The cost
 *                         is texel density: the box grows by one step, so a step of 16 on a
 *                         2048 tile spends ~0.8% of the edge.
 * @return The light-space matrix and this cascade's world-per-texel size.
 */
inline DirShadowFit compute_dir_shadow_fit_sphere(const glm::vec3& light_direction,
                                                  const glm::vec3& centre, float radius,
                                                  float caster_reach, uint32_t tile_resolution,
                                                  uint32_t snap_step_texels = 1u) {
    const glm::vec3 light_dir = glm::normalize(light_direction);
    const glm::vec3 up = (std::abs(light_dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                         : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 light_rot = glm::lookAt(glm::vec3(0.0f), light_dir, up);
    const float resolution = static_cast<float>(std::max(tile_resolution, 1u));
    const glm::vec3 centroid = centre;

    DirShadowFit fit;
    glm::mat4 light_proj;
    const glm::vec3 center_ls = glm::vec3(light_rot * glm::vec4(centroid, 1.0f));

    // Quantize the extent too, so a slowly changing radius doesn't rescale the box (and
    // with it the texel grid the centre snaps to) on every single frame.
    //
    // A snap step past 1 also pads the extent: the snapped centre trails the true one by up
    // to a step on each axis, so without the pad the box would stop covering the far side of
    // its own radius. texel_world depends on the extent which depends on the pad, so the pad
    // is sized from the UNPADDED texel size -- a half-step of it -- which is exact enough
    // that the final box still contains the sphere with room to spare.
    const uint32_t snap = std::max(snap_step_texels, 1u);
    const float pad = 1.0f;
    const float extent_step = 0.5f;
    float extent = std::ceil((radius + pad) / extent_step) * extent_step;
    if (snap > 1u) {
        const float unpadded_texel = (2.0f * extent) / resolution;
        extent = std::ceil((radius + pad + 0.5f * static_cast<float>(snap) * unpadded_texel) /
                           extent_step) * extent_step;
    }

    fit.texel_world = (2.0f * extent) / resolution;
    // Snap to whole SNAP STEPS, not whole texels: a step of 1 is the historical texel snap
    // (which removes sub-texel crawl), and a larger step is what holds the matrix still
    // across frames so the level's depth can be reused.
    const float snap_world = fit.texel_world * static_cast<float>(snap);
    glm::vec2 center(center_ls.x, center_ls.y);
    center.x = std::floor(center.x / snap_world) * snap_world;
    center.y = std::floor(center.y / snap_world) * snap_world;

    // `caster_reach`, not the slice's own depth: a cascade covering the nearest 5 metres
    // must still catch a caster 30 metres up between the sun and that ground, so the
    // towards-light pull stays sized off the whole shadowed range for every cascade.
    //
    // The DEPTH origin is snapped on the same grid as the centre whenever a coarse step is
    // asked for. Without it the matrix would still change every frame that the camera moves
    // along the light direction -- orthoRH_ZO's z row is built from near_plane -- so a
    // level's depth could never be reused however well its xy was snapped. Both planes gain
    // a step of slack so the sphere stays covered while the snapped origin lags.
    const float pad_z = (snap > 1u) ? snap_world : 0.0f;
    float center_z = center_ls.z;
    if (snap > 1u) center_z = std::floor(center_z / snap_world) * snap_world;
    const float far_plane  = -center_z + radius + pad + pad_z;
    float near_plane = -center_z - radius - pad - pad_z - std::max(caster_reach, 0.0f);
    const float depth = std::max(far_plane - near_plane, 0.01f);
    fit.depth_range_world = depth;

    light_proj = glm::orthoRH_ZO(center.x - extent, center.x + extent,
                                 center.y - extent, center.y + extent,
                                 near_plane, near_plane + depth);

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
    // assets/scenes/terrain/terrain_demo (centre ~-185, box half-extent ~79).
    fit.light_space_matrix = light_proj * light_rot;
    return fit;
}
/**
 * @brief Fits an orthographic light-space box to a bounding sphere of one SLICE of the
 *        camera's view frustum, with texel-snapped centering.
 *
 * The technique behind Unity/Unreal's directional "shadow distance", applied per cascade.
 * Two properties matter and both are deliberate:
 *
 *  - **It is a function of the camera alone.** No renderer, SDF or physics position is read,
 *    so nothing in the scene -- including an object that escaped the playable area and is in
 *    unbounded freefall -- can perturb or blow out the shadow frustum.
 *  - **It fits the slice's bounding SPHERE, not its box.** A sphere's radius is invariant
 *    under camera rotation, so the box size is stable as the camera turns; snapping the
 *    centre to whole texels then removes the sub-texel crawl that remains under translation.
 *
 * The near plane sits a further `slice_far` behind the sphere on the towards-light side, so
 * a caster outside the visible sphere but between it and the light still shadows into frame
 * -- sized off the slice's own reach, never off any object's actual position.
 *
 * @param light_direction Directional light's direction; normalized internally.
 * @param cam             Active camera, or nullptr for the fixed fallback box.
 * @param slice_near      Near edge of this cascade's depth slice, in world units.
 * @param slice_far       Far edge of this cascade's depth slice, in world units.
 * @param tile_resolution Edge length of the cascade's atlas TILE in texels; drives the snap.
 * @return The light-space matrix and this cascade's world-per-texel size.
 */
inline DirShadowFit compute_dir_shadow_fit_slice(const glm::vec3& light_direction,
                                                 const ShadowFitCamera* cam,
                                                 float slice_near,
                                                 float slice_far,
                                                 float caster_reach,
                                                 uint32_t tile_resolution) {
    const glm::vec3 light_dir = glm::normalize(light_direction);
    // Any up vector not parallel to the light works; swap axes near the poles of the
    // engine's Z-up convention so lookAt() never degenerates.
    const glm::vec3 up = (std::abs(light_dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                         : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 light_rot = glm::lookAt(glm::vec3(0.0f), light_dir, up);
    const float resolution = static_cast<float>(std::max(tile_resolution, 1u));

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

        const float near_d = std::max(slice_near, cam->near_clip);
        const float far_d  = std::min(cam->far_clip, std::max(slice_far, near_d));

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

        return compute_dir_shadow_fit_sphere(light_direction, centroid, radius, caster_reach,
                                             tile_resolution);
    }

    fit.light_space_matrix = light_proj * light_rot;
    return fit;
}

/**
 * @brief The single-cascade fit: one box over the whole `[near_clip, shadow_distance]`
 *        range, i.e. compute_dir_shadow_fit_slice() with the slice set to everything.
 *
 * What `ToyRenderConfig::shadow_cascades` of 1 produces, and the shape the volumetrics
 * fallback and the no-camera startup path want.
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
    const float slice_near = cam ? cam->near_clip : 0.0f;
    return compute_dir_shadow_fit_slice(light_direction, cam, slice_near, shadow_distance,
                                        shadow_distance, shadow_map_resolution);
}

/**
 * @struct CascadeTile
 * @brief Where one cascade lives in the shadow atlas, in [0,1] ATLAS uv.
 */
struct CascadeTile {
    glm::vec2 origin{0.0f}; /**< uv of the tile's lower-left corner. */
    glm::vec2 scale{1.0f};  /**< Tile size in atlas uv; multiply a [0,1] tile uv by this. */
};

/**
 * @brief The atlas grid a cascade count uses: `grid_x` tiles per row, `grid_y` rows.
 *
 * 1 -> 1x1 (literally today's single map), 2 -> 2x1, 3 and 4 -> 2x2. Never a square grid
 * with unused rows: the atlas image is allocated at `tile * grid_x` by `tile * grid_y`, so
 * a wasted row is wasted VRAM.
 */
inline glm::uvec2 cascade_atlas_grid(uint32_t cascade_count) {
    const uint32_t n = std::clamp<uint32_t>(cascade_count, 1u, 8u);
    // Widest-first, never a row that sits empty: the image is allocated at tile_res * grid,
    // so a wasted row is wasted VRAM. 1..4 are exactly what this returned before the level
    // count grew past 4 (1x1, 2x1, 2x2, 2x2); 6 packs as 3x2 and 8 as 4x2 with no waste.
    const uint32_t columns = n <= 2u ? n : n <= 4u ? 2u : n <= 6u ? 3u : 4u;
    return glm::uvec2(columns, (n + columns - 1u) / columns);
}

/** @brief Cascade `index`'s tile rect in atlas uv, for the grid `cascade_count` implies. */
inline CascadeTile cascade_atlas_tile(uint32_t index, uint32_t cascade_count) {
    const glm::uvec2 grid = cascade_atlas_grid(cascade_count);
    const uint32_t   i    = std::min(index, cascade_count > 0 ? cascade_count - 1u : 0u);
    CascadeTile tile;
    tile.scale  = glm::vec2(1.0f / static_cast<float>(grid.x), 1.0f / static_cast<float>(grid.y));
    tile.origin = glm::vec2(static_cast<float>(i % grid.x), static_cast<float>(i / grid.x)) * tile.scale;
    return tile;
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_SHADOW_FIT_H
