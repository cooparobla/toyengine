#ifndef TOY_PIXEL_SHADOW_BODY_GLSL
#define TOY_PIXEL_SHADOW_BODY_GLSL

// pixel_shadow_body.glsl -- shared calc_dir_shadow()/calc_point_shadow()/calc_spot_shadow()
// body, extracted from four byte-identical copies (pixel_lighting.frag,
// pixel_forward_shading.glsl's gfx_forward_calc_* pair, gfx/surface/capture_fs.glsl,
// sdf_capture.frag) so opaque, forward/BLEND, capture and SDF surfaces can never
// silently diverge on shadow behavior the way transparent_capture.frag's
// band()/shade_light() duplicate already had to document doing.
//
// A "body" file in the pixel_forward_shading.glsl/ssr_trace_body.glsl sense: the
// includer must, BEFORE including this file, already have declared (with these
// exact names, whatever their own set/binding indices are):
//   - `lights`            -- the LightUBO block (dir_shadow_params/dir_shadow_extra/
//                             dir_cascade_matrix and the other dir_cascade_* fields/
//                             spot_shadow_params/spot_light_space_matrix -- see
//                             light_data.h's C++ doc for the packing of these).
//   - `dir_shadow_map`    -- sampler2DShadow. The directional cascade ATLAS, not a
//                            single map -- calc_dir_shadow() does the tile remap.
//   - `dir_shadow_map_raw`-- sampler2D: the SAME directional atlas bound a second
//                            time through a plain nearest sampler, for the PCSS
//                            blocker search (a compare sampler cannot return the
//                            stored depth the search averages).
//   - `point_shadow_map`  -- samplerCubeShadow.
//   - `spot_shadow_map`   -- sampler2DShadow.
//   - #include <gfx/shadow_sampling.glsl> (for gfx_csm_select/gfx_csm_atlas_coords,
//     gfx_shadow_dir_hard/_pcf_vogel, gfx_shadow_dir_pcss, gfx_shadow_cube_hard/
//     _pcf_vogel, gfx_ign_angle).

// Directional shadow, against the CASCADED shadow atlas: hard compare when this cascade's
// PCF radius (dir_cascade_pcf_texels, in atlas texels) is 0, else a rotated Vogel-disk PCF
// penumbra -- see PixelRenderConfig::soft_shadows/shadow_softness and
// update_dir_shadow_matrix_() for how that radius is derived from world-space softness, per
// cascade, each frame.
//
// Takes the RAW shading point, not a precomputed light-space position, and applies the
// normal-offset bias itself. Both are consequences of cascades: the offset is measured in
// shadow-map texels (compute_shadow_normal_bias()) and every cascade has its own texel size,
// so the caller cannot know which offset to apply until the cascade is chosen -- and the
// cascade is chosen from the point's own world position. Selecting on the UNBIASED position
// is deliberate: the offset is centimetres, far too small to change the choice anywhere but
// exactly on a boundary, where either cascade is equally correct.
float calc_dir_shadow(vec3 world_pos, vec3 N, vec3 L) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    // IGN, not a fract(sin(...)) hash: a pixel-frequency hash puts its error exactly
    // where TAA resolves worst (a jittered reprojection lands on an equally-random
    // neighbour); IGN's smooth-ramp-per-tile structure reads as fine dither instead.
    // The frame offset (dir_shadow_extra.w, sourced from frame_index_) is what then
    // gives TAA's history buffer a genuinely different sample set to average in over
    // time -- the same golden-angle trick ssao.frag's noise_rotation already uses.
    float angle = gfx_ign_angle(gl_FragCoord.xy) + lights.dir_shadow_extra.w * 2.39996323;
    // The same value as a [0,1) draw, reused to dither the cascade transition: one pixel
    // near a cascade boundary takes the coarser tile, its neighbour the finer, and TAA
    // averages the pair into a gradient instead of leaving a visible resolution seam.
    float cascade_dither = fract(angle * 0.15915494); // angle / 2pi

    int cascade = gfx_csm_select(lights.dir_cascade_matrix, lights.dir_cascade_info,
                                 world_pos, cascade_dither);
    // Past the last cascade -- beyond shadow_distance, or off to the side of every tile.
    // Unshadowed, the same answer a single map gives outside its own frustum.
    if (cascade < 0) return 0.0;

    // Normal-offset shadows: push the sample off its own surface along the geometric normal
    // before projecting, by this cascade's own world-unit offset. Scaled up as the surface
    // tilts away from the light, which is where one shadow texel covers the most surface and
    // the acne it causes is worst.
    float normal_bias_scale = clamp(1.0 - dot(N, L), 0.0, 1.0);
    vec3  biased_pos = world_pos +
        N * (lights.dir_cascade_normal_bias[cascade] * (0.5 + 0.5 * normal_bias_scale));
    vec3 proj_coords = gfx_csm_atlas_coords(lights.dir_cascade_matrix, lights.dir_cascade_info,
                                            biased_pos, cascade);

    float bias = max(lights.dir_shadow_params.x * (1.0 - max(dot(N, L), 0.0)), 0.0002);
    float radius_texels = lights.dir_cascade_pcf_texels[cascade];

    float shadow;
    if (radius_texels <= 0.0) {
        shadow = gfx_shadow_dir_hard(dir_shadow_map, proj_coords, bias);
    } else if (lights.pcss_params.x > 0.5) {
        // PCSS contact hardening: the constant radius above becomes the penumbra's
        // MAX; the blocker search shrinks it toward the contact point. See
        // gfx_shadow_dir_pcss's doc for the directional (linear-in-gap) penumbra model.
        // The penumbra-per-depth-gap factor is per cascade, since it divides by that
        // cascade's texel size, but the search radius and tap budget are shared.
        shadow = gfx_shadow_dir_pcss(dir_shadow_map, dir_shadow_map_raw, proj_coords, bias,
                                     lights.dir_cascade_pcss_scale[cascade], lights.pcss_params.z,
                                     radius_texels, angle, int(lights.dir_shadow_extra.z),
                                     int(lights.pcss_params.w));
    } else {
        // textureSize() is the whole ATLAS, which is exactly right: a tile texel and an
        // atlas texel are the same physical texel, so a radius in texels converts to uv
        // against the atlas without any per-tile correction.
        vec2 texel_size = radius_texels / textureSize(dir_shadow_map, 0);
        shadow = gfx_shadow_dir_pcf_vogel(dir_shadow_map, proj_coords, bias, texel_size,
                                          angle, int(lights.dir_shadow_extra.z));
    }
    // Per-light darkness (DirectionalLightComponent::shadow_intensity), applied HERE
    // rather than at each (1.0 - shadow) call site, so every shading path inherits it
    // for free instead of needing its own multiply.
    return shadow * lights.dir_shadow_extra.x;
}

// Point shadow: hard compare when dir_shadow_extra.y (PCF tangent-offset radius, already
// converted from PixelRenderConfig::point_shadow_softness texels on the CPU -- see
// pixel_render_pipeline.h's update_scene_lighting_ubo) is 0, else a rotated Vogel-disk
// PCF penumbra in the tangent plane perpendicular to the sample direction (see
// gfx_shadow_cube_pcf_vogel's doc for why that, and not the corner-rotation
// gfx_shadow_cube_pcf, is the kernel used here). Only lights.point_lights[0] can be a
// real shadow caster -- toyengine's ShadowMapTarget holds exactly one cube map at a time.
// `frag_to_light` here is surface-to-light (negated at the point of sampling, since the
// cube map was rendered looking outward FROM the light -- see shadow_cube.vert/frag).
float calc_point_shadow(vec3 frag_to_light, float range) {
    vec3 dir = normalize(-frag_to_light);
    float current_dist = length(frag_to_light) / range;
    float bias = 0.05 / range;

    float disk_radius = lights.dir_shadow_extra.y;
    if (disk_radius <= 0.0) return gfx_shadow_cube_hard(point_shadow_map, dir, current_dist, bias);

    float angle = gfx_ign_angle(gl_FragCoord.xy) + lights.dir_shadow_extra.w * 2.39996323;
    return gfx_shadow_cube_pcf_vogel(point_shadow_map, dir, current_dist, bias, disk_radius,
                                     angle, int(lights.dir_shadow_extra.z));
}

// Spot shadow: same structure as calc_dir_shadow() (single perspective frustum, sampler2DShadow,
// hard compare when spot_shadow_params.y is 0 else a rotated Vogel-disk PCF penumbra) since a
// spot map -- unlike the point light's cube map -- is one frustum too. Reuses dir_shadow_extra's
// PCF tap count (.z) and per-frame rotation offset (.w) rather than duplicating them into
// spot_shadow_params, since both maps share the same TAA-decorrelation scheme. Only
// lights.spot_lights[light_counts.w] can be a real shadow caster -- toyengine's ShadowMapTarget
// holds exactly one spot 2D map at a time, same one-caster rule as point lights' cube map.
//
// spot_shadow_params.y is a distance-scaled penumbra factor, not a raw texel radius: the spot
// map is a perspective projection, so one texel covers 2*d*tan(outer_half)/resolution world
// units at forward distance d from the light. The CPU stores
// K = softness_world * resolution / (2*tan(outer_half)) (see update_spot_shadow_matrix_()),
// and dividing by light_space_pos.w (= d for this matrix) yields the texel radius that keeps
// the penumbra a constant WORLD width at every receiver distance -- the same world-space
// behavior calc_dir_shadow() gets from its per-frame texel_world conversion. Clamped to the
// same 12-texel practical limit as the directional radius, per-pixel since d varies.
float calc_spot_shadow(vec4 light_space_pos, vec3 N, vec3 L) {
    if (lights.spot_shadow_params.z < 0.5) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    if (proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) {
        return 0.0;
    }

    float bias = max(lights.spot_shadow_params.x * (1.0 - max(dot(N, L), 0.0)), 0.0002);
    float radius_texels = lights.spot_shadow_params.y <= 0.0 ? 0.0
        : min(lights.spot_shadow_params.y / max(light_space_pos.w, 1e-4), 12.0);

    if (radius_texels <= 0.0) {
        return gfx_shadow_dir_hard(spot_shadow_map, proj_coords, bias);
    }
    // See calc_dir_shadow()'s identical IGN-vs-hash rationale.
    float angle = gfx_ign_angle(gl_FragCoord.xy) + lights.dir_shadow_extra.w * 2.39996323;
    vec2 texel_size = radius_texels / textureSize(spot_shadow_map, 0);
    return gfx_shadow_dir_pcf_vogel(spot_shadow_map, proj_coords, bias, texel_size,
                                    angle, int(lights.dir_shadow_extra.z));
}

#endif // TOY_PIXEL_SHADOW_BODY_GLSL
