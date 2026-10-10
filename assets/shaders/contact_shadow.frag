#version 450

// contact_shadow.frag -- the screen-space contact-shadow march, as its own full-screen pass.
//
// Runs contact_shadow_body.glsl's single-ray depth-buffer march once per pixel. Having its own
// pass buys two things an inline march in the lighting shader could not have:
//
//  1. A BUFFER to filter. The march is a short stochastic-ish trace over G-buffer texels, and
//     inline it would have nowhere to accumulate -- its only temporal filter would be the TAA,
//     whose variance clip rejects history exactly on thin, high-contrast features under motion,
//     which is what a contact shadow is. The pass writes an occlusion buffer that the shared
//     temporal resolve then averages over contact_shadow_temporal_frames draws.
//  2. One evaluation per pixel instead of one per pixel per light -- academic today (the term is
//     directional-only) but it is why HDRP computes contact shadows into their own texture too.
//
// The output is the RAW geometric occlusion the body returns, unscaled by strength or per-light
// darkness: the consumer max()-combines and scales, exactly as before, so shadows_enabled: false
// plus contact_shadows_enabled: true still renders a contact-only view.
//
// Set layout matches toy_lighting.frag's first two sets plus its G-buffer set, so the march
// resolves the identical `camera` / `lights` / `g_*` names its include contract requires.

#include <gfx/spot_light.glsl>
#include <gfx/ssr_common.glsl>

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_occlusion;

// Set 0: Camera UBO -- identical layout to toy_lighting.frag's.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- see light_ubo_body.glsl, the single copy of this block's layout.
#include "light_ubo_body.glsl"

// Set 2: G-Buffer. Only normal and position are needed; g_albedo_ao is declared so the set
// layout stays the three-binding shape every other G-buffer consumer in this engine uses.
layout(set = 2, binding = 0) uniform sampler2D g_albedo_ao;
layout(set = 2, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 2, binding = 2) uniform sampler2D g_position_roughness;
// The rasterised scene depth: the march's one fetch per step (ContactShadowPass binding 3).
layout(set = 2, binding = 3) uniform sampler2D u_scene_depth;

// Must follow the declarations above, not precede them -- the same required-before-include
// contract gfx/ssr_trace_body.glsl documents for itself.
#include "contact_shadow_body.glsl"

/// This pixel's VIEW-space surface point from the rasterised depth, through the closed-form
/// inverse of the JITTERED projection the depth was rasterised with (the same reconstruction
/// ssao.frag uses), for either projection kind.
vec3 view_pos_from_depth(vec2 uv, float depth) {
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0));
    float c22 = camera.proj[2][2];
    float c32 = camera.proj[3][2];
    if (camera.proj[2][3] != 0.0) {
        float z = -c32 / (depth + c22);
        return vec3(-z * (ndc.x + camera.proj[2][0]) / camera.proj[0][0],
                    -z * (ndc.y + camera.proj[2][1]) / camera.proj[1][1], z);
    }
    return vec3((ndc.x - camera.proj[3][0]) / camera.proj[0][0],
                (ndc.y - camera.proj[3][1]) / camera.proj[1][1],
                (depth - c32) / c22);
}

void main() {
    ivec2 gsize = textureSize(u_scene_depth, 0);
    ivec2 px    = clamp(ivec2(gl_FragCoord.xy), ivec2(0), gsize - 1);
    vec2  uv    = (vec2(px) + 0.5) / vec2(gsize);
    float depth = texelFetch(u_scene_depth, px, 0).r;
    vec3  P     = view_pos_from_depth(uv, depth);

    // Evaluated for EVERY fragment, before the background branch below: derivatives are
    // undefined under non-uniform control flow. P is smooth across a surface (an exact function
    // of depth), so its derivatives give the surface's geometric normal and footprint.
    vec3 dpdx = dFdx(P);
    vec3 dpdy = dFdy(P);

    vec3 N_raw = texelFetch(g_normal_metallic, px, 0).rgb;
    // Background pixels write N = vec3(0): nothing to shadow, and normalize(0) is NaN.
    if (depth >= 1.0 || dot(N_raw, N_raw) < 0.001) {
        out_occlusion = vec4(0.0);
        return;
    }
    vec3 N = normalize(mat3(camera.view) * normalize(N_raw));

    // Geometric normal, oriented toward the camera; across a depth edge the derivative quad
    // spans two surfaces and the cross product is meaningless, so fall back to the shading
    // normal wherever the two disagree badly.
    vec3 Ng = cross(dpdy, dpdx);
    float ng_len = length(Ng);
    Ng = ng_len > 1e-12 ? Ng / ng_len : N;
    if (dot(Ng, -P) < 0.0) Ng = -Ng;
    if (dot(Ng, N) < 0.5) Ng = N;

    vec3 L = normalize(mat3(camera.view) * -lights.dir_direction.xyz);

    float occlusion = toy_contact_shadow(P, N, Ng, L, dpdx, dpdy);

    // Left in .r alone, NOT replicated across rgb: three independent YCoCg clips in the shared
    // resolve would bound the scalar more loosely than one channel's clip does.
    out_occlusion = vec4(occlusion, 0.0, 0.0, 0.0);
}
