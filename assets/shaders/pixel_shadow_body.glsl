#ifndef TOY_PIXEL_SHADOW_BODY_GLSL
#define TOY_PIXEL_SHADOW_BODY_GLSL

// pixel_shadow_body.glsl -- shared calc_dir_shadow()/calc_point_shadow() body,
// extracted from four byte-identical copies (pixel_lighting.frag,
// pixel_forward_shading.glsl's gfx_forward_calc_* pair, gfx/surface/capture_fs.glsl,
// sdf_capture.frag) so opaque, forward/BLEND, capture and SDF surfaces can never
// silently diverge on shadow behavior the way transparent_capture.frag's
// band()/shade_light() duplicate already had to document doing.
//
// A "body" file in the pixel_forward_shading.glsl/ssr_trace_body.glsl sense: the
// includer must, BEFORE including this file, already have declared (with these
// exact names, whatever their own set/binding indices are):
//   - `lights`            -- the LightUBO block (dir_shadow_params/dir_shadow_extra/
//                             dir_light_space_matrix -- see light_data.h's C++ doc
//                             for the packing of both vec4s).
//   - `dir_shadow_map`    -- sampler2DShadow.
//   - `point_shadow_map`  -- samplerCubeShadow.
//   - #include <gfx/shadow_sampling.glsl> (for gfx_shadow_dir_hard/_pcf_vogel,
//     gfx_shadow_cube_hard/_pcf_vogel, gfx_ign_angle).

// Directional shadow: hard compare when dir_shadow_params.y (PCF radius in shadow-map
// texels) is 0, else a rotated Vogel-disk PCF penumbra -- see
// PixelRenderConfig::soft_shadows/shadow_softness and update_dir_shadow_matrix_()
// for how that radius is derived from world-space softness each frame.
float calc_dir_shadow(vec4 light_space_pos, vec3 N, vec3 L) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    if (proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) {
        return 0.0;
    }

    float bias = max(lights.dir_shadow_params.x * (1.0 - max(dot(N, L), 0.0)), 0.0002);
    float radius_texels = lights.dir_shadow_params.y;

    float shadow;
    if (radius_texels <= 0.0) {
        shadow = gfx_shadow_dir_hard(dir_shadow_map, proj_coords, bias);
    } else {
        // IGN, not a fract(sin(...)) hash: a pixel-frequency hash puts its error exactly
        // where TAA resolves worst (a jittered reprojection lands on an equally-random
        // neighbour); IGN's smooth-ramp-per-tile structure reads as fine dither instead.
        // The frame offset (dir_shadow_extra.w, sourced from frame_index_) is what then
        // gives TAA's history buffer a genuinely different sample set to average in over
        // time -- the same golden-angle trick ssao.frag's noise_rotation already uses.
        float angle = gfx_ign_angle(gl_FragCoord.xy) + lights.dir_shadow_extra.w * 2.39996323;
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

#endif // TOY_PIXEL_SHADOW_BODY_GLSL
