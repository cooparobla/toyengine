#version 450

// Traced screen-space global illumination: one cosine-weighted hemisphere ray per
// pixel per frame, marched against the SAME Hi-Z pyramid and prefiltered
// scene-colour chain ssr.frag's specular trace uses (gfx/ssr_trace_body.glsl's
// gfx_ssr_trace_dir). The output feeds SsrPass's second temporal-resolve chain and
// is consumed by the composite's diffuse-bounce term (gfx/ssr_composite_body.glsl)
// in place of its single normal-offset mip tap.
//
// Monte-Carlo normalization: with cosine-weighted sampling, the average of the
// sampled radiance IS the irradiance integral over pi -- so the Lambertian bounce
// downstream is simply albedo * E[hit radiance], no extra pi factor anywhere. The
// per-frame IGN jitter makes consecutive frames sample different hemisphere
// directions; the variance-clipped temporal resolve integrates them.
//
// Set layout is ssr.frag's own sets 0-3 (bound to the identical descriptor sets by
// SsrPass::execute()).

#include <gfx/ssr_common.glsl>

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_ssgi_color;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer (binding 0, g_albedo_ao, exists in the layout but is not needed here)
layout(set = 1, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 1, binding = 3) uniform sampler2D u_velocity;   // G4 -- see gfx_ssr_hit_color()

// Set 2: Hi-Z map
layout(set = 2, binding = 0) uniform sampler2D u_hiz_map;

// Set 3: prefiltered scene-colour mip chain
layout(set = 3, binding = 0) uniform sampler2D u_scene_color;

// Only colour * confidence is used below, so a zero-weight ray may skip its march outright.
#define GFX_SSR_SKIP_ZERO_WEIGHT
#include <gfx/ssr_trace_body.glsl>

// Same block as ssr.frag's (SsrPass pushes one SsrPushConstants struct for both
// pipelines); the specular-only fields at the tail are meaningless here.
layout(push_constant) uniform SsrPushConstants {
    mat4  inv_proj;
    float max_distance;      // ssgi_max_distance -- swapped in by SsrPass::execute()
    float bias_texels;
    float thickness_min;
    float thickness_scale;
    float roughness_cutoff;  // pushed above 1 so the trace's roughness fade stays 1
    int   max_iterations;
    int   max_hiz_mip;
    int   start_mip;
    int   min_mip0_steps;
    int   max_color_mip;
    float jitter_strength;
    int   frame_index;
    int   flags;             // bit 0: u_scene_color is the previous frame
    int   rays_per_pixel;    // unused here (always one cosine ray)
    float cone_prefilter;    // 1: diffuse hits read the wide lobe-cone mip
    float skip_threshold;    // unused here
} u_ssr;

void main() {
    ivec2 gsize     = textureSize(g_position_roughness, 0);
    ivec2 origin_px = clamp(ivec2(in_uv * vec2(gsize)), ivec2(0), gsize - 1);

    vec4 norm_met  = texelFetch(g_normal_metallic,    origin_px, 0);
    vec3 N = norm_met.rgb;

    // Background: zero normal, and normalize(vec3(0)) is NaN -- same ordering rule
    // as ssr.frag's early-out.
    if (dot(N, N) < 0.001) {
        out_ssgi_color = vec4(0.0);
        return;
    }
    N = normalize(N);
    vec3 P = texelFetch(g_position_roughness, origin_px, 0).rgb;

    // Cosine-weighted hemisphere direction around N (Malley's method on an IGN
    // pair, golden-angle-shifted per frame -- the same decorrelation ssr.frag's
    // jitter uses, so the temporal resolve sees a fresh direction every frame).
    vec2  xi  = ssr_ign2(gl_FragCoord.xy, u_ssr.frame_index);
    float phi = 6.28318530718 * xi.y;
    float sr  = sqrt(xi.x);
    vec3 up = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t  = normalize(cross(up, N));
    vec3 b  = cross(N, t);
    vec3 dir = normalize(t * (sr * cos(phi)) + b * (sr * sin(phi))
                         + N * sqrt(max(1.0 - xi.x, 0.0)));

    GfxSsrParams sp;
    sp.max_distance    = u_ssr.max_distance;
    sp.bias_texels     = u_ssr.bias_texels;
    sp.thickness_min   = u_ssr.thickness_min;
    sp.thickness_scale = u_ssr.thickness_scale;
    sp.roughness_cutoff = u_ssr.roughness_cutoff;
    sp.max_iterations  = u_ssr.max_iterations;
    sp.max_hiz_mip     = u_ssr.max_hiz_mip;
    sp.start_mip       = u_ssr.start_mip;
    sp.min_mip0_steps  = u_ssr.min_mip0_steps;
    sp.max_color_mip   = u_ssr.max_color_mip;
    sp.jitter_strength = 0.0;   // the hemisphere sample above IS the jitter
    sp.frame_index     = u_ssr.frame_index;
    sp.prev_frame_color = (u_ssr.flags & 1) != 0;
    sp.skip_behind      = (u_ssr.flags & 2) != 0;
    sp.rays_per_pixel   = 1;
    sp.cone_prefilter   = u_ssr.cone_prefilter;

    // Roughness 0.9: the cone-footprint mip selection then reads the coarse end of
    // the prefiltered chain, which is exactly what a diffuse gather wants -- each
    // hit returns a wide neighbourhood average rather than one sharp texel, cutting
    // the estimator's variance at no extra taps.
    GfxSsrHit hit = gfx_ssr_trace_dir(P, N, dir, 0.9, u_ssr.inv_proj, sp);

    out_ssgi_color = vec4(hit.color, hit.confidence);
}
