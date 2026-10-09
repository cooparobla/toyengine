// motion_blur_common.glsl -- shared by the motion blur passes (toyengine/render/passes/motion_blur_pass.h).
//
// One question for every stage: how far, in render pixels, is this pixel smeared during the shutter,
// and how deep is it? Both come from the G-buffer's velocity attachment (G4, gfx/surface/gbuffer_fs.glsl):
// xy = this frame's UV minus last frame's (camera AND object motion, unjittered), w = linear view
// depth (0 = nothing drawn). The sky writes nothing there, so its motion is rebuilt from the camera
// alone: the far-plane point under the pixel, reprojected through `sky_reproject` (last frame's
// unjittered view-projection times the inverse of this frame's).
//
// The result is a HALF vector: the pixel is spread over [-v, +v], so `scale` is 0.5 * shutter fraction
// times the render extent; its length is clamped to the max radius.

#ifndef MOTION_BLUR_COMMON_GLSL
#define MOTION_BLUR_COMMON_GLSL

/// Depth standing in for "no surface" (the sky): behind everything.
const float MB_SKY_DEPTH = 1.0e6;

vec2 mb_ndc_to_uv(vec2 ndc) { return vec2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5); }
vec2 mb_uv_to_ndc(vec2 uv)  { return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }

/// xy = blur half vector in pixels (clamped to max_radius), z = linear depth (MB_SKY_DEPTH for the sky).
vec3 mb_velocity_depth(sampler2D tex_velocity, ivec2 p, vec2 extent, mat4 sky_reproject,
                       float scale, float max_radius, bool sky_valid) {
    vec4 g = texelFetch(tex_velocity, p, 0);
    vec2 v_uv;
    float z;
    if (g.w > 0.0) {
        v_uv = g.xy;
        z    = g.w;
    } else {
        v_uv = vec2(0.0);
        z    = MB_SKY_DEPTH;
        if (sky_valid) {
            vec2 uv   = (vec2(p) + 0.5) / extent;
            vec4 prev = sky_reproject * vec4(mb_uv_to_ndc(uv), 1.0, 1.0);
            if (prev.w > 0.0) v_uv = uv - mb_ndc_to_uv(prev.xy / prev.w);
        }
    }
    vec2  v   = v_uv * extent * scale;
    float len = length(v);
    if (len > max_radius) v *= max_radius / len;
    return vec3(v, z);
}

#endif // MOTION_BLUR_COMMON_GLSL
