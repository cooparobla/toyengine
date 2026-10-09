#version 450

// motion_blur_gather.frag -- motion blur stage 3 (toyengine/render/passes/motion_blur_pass.h).
//
// McGuire-style reconstruction (McGuire, Hennessy, Bukowski, Osman 2012), gathered along the
// tile neighbourhood's dominant velocity. Every sample y on the segment x +- v_n asks whether its
// colour reaches x during the shutter, with depth deciding who covers whom:
//
//   * y IN FRONT of x and y's own smear reaches x     -> a foreground object blurring over x;
//   * y BEHIND x and x's own smear reaches y          -> x is moving, so the background behind
//                                                       its path shows through at x.
//
// Both tests are box-shutter cylinders (|x - y| < |v|) with a soft edge, and the depth compare is
// soft and relative to depth so a surface never splits against itself. A sample that answers no to
// both stands for "x itself" (its share stays the centre colour), which is what keeps a static
// object in front of a moving one sharp and stops a static background from gathering the object
// beside it. Positions are stratified along the segment and jittered per pixel (interleaved
// gradient noise), as is the tile lookup, which hides the tile grid.
//
// A pixel whose neighbourhood moves less than half a pixel returns its colour bit-for-bit.

#include "motion_blur_common.glsl"

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex_color;      // copy of the HDR scene colour, nearest
layout(set = 0, binding = 1) uniform sampler2D tex_velocity;   // G4, nearest
layout(set = 0, binding = 2) uniform sampler2D tex_neighbor;   // NeighborMax, nearest

layout(push_constant) uniform GatherParams {
    mat4  sky_reproject;
    vec4  extent_scale;   // xy = render extent, z = 0.5 * shutter, w = max radius (px)
    ivec4 info;           // x = tile size, y = sky reprojection valid, z = samples, w = noise frame
} pc;

/// Jimenez 2014 interleaved gradient noise, in [0, 1).
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

/// 1 where a is in front of b or level with it, 0 where clearly behind; the band is 5% of depth.
float soft_front(float za, float zb) {
    return clamp(1.0 - (za - zb) / (0.05 * min(za, zb) + 1e-4), 0.0, 1.0);
}

/// 1 inside a smear of half length l, 0 outside it, over a soft edge of about a pixel.
float reaches(float dist, float l) {
    return clamp((l - dist) / max(0.1 * l, 1.0) + 0.5, 0.0, 1.0);
}

void main() {
    const ivec2 size   = ivec2(pc.extent_scale.xy);
    const ivec2 p      = ivec2(gl_FragCoord.xy);
    const vec4  center = texelFetch(tex_color, p, 0);

    const vec2 noise_pos = vec2(p) + 5.588238 * float(pc.info.w);
    const float j0 = ign(noise_pos);
    const float j1 = ign(noise_pos + vec2(47.0, 17.0));

    // Neighbourhood velocity, looked up through a tile index jittered by up to a quarter tile.
    const int   tile  = pc.info.x;
    const ivec2 grid  = textureSize(tex_neighbor, 0);
    vec2  jitter = (vec2(j0, j1) - 0.5) * (0.5 * float(tile));
    ivec2 t      = clamp(ivec2((vec2(p) + 0.5 + jitter) / float(tile)), ivec2(0), grid - 1);
    vec2  vn     = texelFetch(tex_neighbor, t, 0).xy;
    float ln     = length(vn);
    if (ln < 0.5) {
        out_color = center;
        return;
    }

    const bool sky_ok = pc.info.y != 0;
    vec3  c  = mb_velocity_depth(tex_velocity, p, pc.extent_scale.xy, pc.sky_reproject,
                                 pc.extent_scale.z, pc.extent_scale.w, sky_ok);
    float lc = length(c.xy);
    float zc = c.z;

    const int samples = max(pc.info.z, 2);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < samples; ++i) {
        float s    = mix(-1.0, 1.0, (float(i) + j0) / float(samples));
        vec2  off  = vn * s;
        ivec2 q    = clamp(ivec2(vec2(p) + 0.5 + off), ivec2(0), size - 1);
        float dist = length(off);

        vec3  y  = mb_velocity_depth(tex_velocity, q, pc.extent_scale.xy, pc.sky_reproject,
                                     pc.extent_scale.z, pc.extent_scale.w, sky_ok);
        float ly = length(y.xy);

        float w = max(soft_front(y.z, zc) * reaches(dist, ly),
                      soft_front(zc, y.z) * reaches(dist, lc));
        acc += w * (texelFetch(tex_color, q, 0).rgb - center.rgb);
    }
    out_color = vec4(center.rgb + acc / float(samples), center.a);
}
