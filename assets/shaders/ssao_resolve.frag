#version 450

#include <gfx/ssr_common.glsl>

// Temporal resolve for the raw SSAO hemisphere-sample output (scalar clone of ssr_resolve.frag --
// see that file for the reasoning behind reprojecting via world position rather than motion
// vectors, and why the neighbourhood clamp stays even with real reprojection).

layout(location = 0) in  vec2  in_uv;
layout(location = 0) out float out_ao;

layout(set = 0, binding = 0) uniform sampler2D u_ao_current;          // raw AO, NEAREST
layout(set = 0, binding = 1) uniform sampler2D u_ao_history;          // LINEAR (reprojected UV)
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;  // NEAREST
layout(set = 0, binding = 3) uniform sampler2D g_normal_metallic;     // NEAREST -- background test

layout(push_constant) uniform PushConstants {
    mat4  prev_view_proj;   // previous frame's JITTERED proj * view
    // Flattened vec2, matching the house rule in ssao.frag's SsaoPushConstants.
    float resolution_x;
    float resolution_y;
    float blend_factor;
    int   history_valid;    // 0 until both a history image and a previous matrix exist
} pc;

void main() {
    float current = texture(u_ao_current, in_uv).r;

    // Background: the raw pass already wrote 1.0 here and G2 holds no real surface to
    // reproject, so don't even attempt it.
    vec3 N = texture(g_normal_metallic, in_uv).rgb;
    if (dot(N, N) < 0.001) {
        out_ao = current;
        return;
    }

    if (pc.history_valid == 0) {
        out_ao = current;
        return;
    }

    vec3 P = texture(g_position_roughness, in_uv).rgb;
    vec4 prev_clip = pc.prev_view_proj * vec4(P, 1.0);

    // Behind the previous frame's eye: no history exists for this point at all.
    if (prev_clip.w <= 0.0) {
        out_ao = current;
        return;
    }

    vec2 prev_uv = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);

    // Off-screen last frame -- the cheapest and most common disocclusion case.
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_ao = current;
        return;
    }

    // 3x3 neighbourhood min/max of the CURRENT frame's raw AO, computed after the early-outs
    // above (matching ssr_resolve.frag's ordering).
    vec2 texel_size = 1.0 / vec2(pc.resolution_x, pc.resolution_y);
    float s0 = texture(u_ao_current, in_uv + vec2(-1.0,  1.0) * texel_size).r;
    float s1 = texture(u_ao_current, in_uv + vec2( 0.0,  1.0) * texel_size).r;
    float s2 = texture(u_ao_current, in_uv + vec2( 1.0,  1.0) * texel_size).r;
    float s3 = texture(u_ao_current, in_uv + vec2(-1.0,  0.0) * texel_size).r;
    float s4 = current;
    float s5 = texture(u_ao_current, in_uv + vec2( 1.0,  0.0) * texel_size).r;
    float s6 = texture(u_ao_current, in_uv + vec2(-1.0, -1.0) * texel_size).r;
    float s7 = texture(u_ao_current, in_uv + vec2( 0.0, -1.0) * texel_size).r;
    float s8 = texture(u_ao_current, in_uv + vec2( 1.0, -1.0) * texel_size).r;

    float aabb_min = min(s0, min(s1, min(s2, min(s3, min(s4, min(s5, min(s6, min(s7, s8))))))));
    float aabb_max = max(s0, max(s1, max(s2, max(s3, max(s4, max(s5, max(s6, max(s7, s8))))))));

    float history = texture(u_ao_history, prev_uv).r;
    float clamped_history = clamp(history, aabb_min, aabb_max);

    out_ao = mix(current, clamped_history, pc.blend_factor);
}
