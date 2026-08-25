#version 450

// Hi-Z screen-space reflection raymarch. Thin wrapper around
// gfx/ssr_trace_body.glsl's gfx_ssr_trace() -- this shader only reads the
// camera UBO, G-buffer, Hi-Z depth pyramid and prefiltered scene-colour mip
// chain, none of which differ between blendy's GI-enabled pipeline and
// toyengine's GI-free one, so nothing here needs to differ from gfxcoopa's
// own copy of this file either. (This file used to carry its own inlined
// copy of the raymarch, predating the gfx_ssr_trace() extraction -- since
// ShaderLibrary resolves toyengine's own assets/shaders/ before gfxcoopa's,
// THIS file, not gfxcoopa's, is what actually built and ran; keeping the two
// in sync by hand was exactly the risk factoring out gfx_ssr_trace() was
// meant to remove, so this file now just includes it like every other
// consumer.) Only the composite (ssr_composite.frag) drops the
// reflection-probe/GI descriptor set.

#include <gfx/ssr_common.glsl>

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_ssr_color;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer
layout(set = 1, binding = 0) uniform sampler2D g_albedo_ao;
layout(set = 1, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 2) uniform sampler2D g_position_roughness;

// Set 2: Hi-Z map
layout(set = 2, binding = 0) uniform sampler2D u_hiz_map;

// Set 3: Deferred Lit Scene Color (prefiltered mip chain as of Phase 4)
layout(set = 3, binding = 0) uniform sampler2D u_scene_color;

// Sets 4-6: SECOND, independent trace source (e.g. a forward capture of transparent
// geometry -- see gfx/ssr_trace_secondary_body.glsl's doc). Always declared and bound
// (to a permanent 1x1 neutral fallback when unused -- see SsrPass::set_secondary_source())
// so the pipeline layout stays valid regardless of the runtime has_secondary toggle below;
// only the dynamic branch in main() is conditional.
layout(set = 4, binding = 0) uniform sampler2D g_normal_metallic_b;
layout(set = 4, binding = 1) uniform sampler2D g_position_roughness_b;
layout(set = 5, binding = 0) uniform sampler2D u_hiz_map_b;
layout(set = 6, binding = 0) uniform sampler2D u_scene_color_b;

// gfx_ssr_trace()'s required-before-include contract (camera/g_normal_metallic/
// g_position_roughness/u_hiz_map/u_scene_color) is satisfied by the declarations above --
// must include AFTER them, not before (matches transparent.frag's own ordering). Same for
// gfx_ssr_trace_secondary()'s _b-suffixed contract.
#include <gfx/ssr_trace_body.glsl>
#include <gfx/ssr_trace_secondary_body.glsl>

layout(push_constant) uniform SsrPushConstants {
    mat4  inv_proj;
    float max_distance;      // world-space ray length (ssr_max_distance)
    float bias_texels;       // normal bias, in full-res screen texels (ssr_bias_texels)
    float thickness_min;     // world-space floor for the thickness test (ssr_thickness)
    float thickness_scale;   // thickness as a fraction of |view z| (ssr_thickness_scale)
    float roughness_cutoff;
    int   max_iterations;
    int   max_hiz_mip;
    int   start_mip;         // Hi-Z mip the march starts at (ssr_start_mip)
    int   min_mip0_steps;    // self-reflection gate (ssr_min_mip0_steps)
    int   max_color_mip;     // top mip of the prefiltered scene-colour chain

    // Secondary source: != 0 -> also trace gfx_ssr_trace_secondary() and keep whichever hit
    // has the smaller GfxSsrHit.travel. Every other GfxSsrParams field above (distance/bias/
    // thickness/roughness_cutoff/iterations/start_mip/min_mip0_steps) is shared between the
    // two traces -- only the two pyramids' own mip counts can legitimately differ, since
    // they're independent HiZPass/SceneColorMipPass instances over independently-sized
    // image chains (see PixelRenderPipeline's transparent_hiz_pass_/transparent_scene_color_
    // mip_pass_).
    float has_secondary;
    int   max_hiz_mip_b;
    int   max_color_mip_b;
} u_ssr;

void main() {
    // Snap to a definite full-res G-buffer texel. At full-res tracing in_uv is already texel-
    // centred and this is the identity; under half-res tracing (Phase 5) in_uv lands on a
    // full-res texel *corner*, where a NEAREST sampler picks one of two texels by float
    // rounding. Deriving an explicit integer coordinate keeps one code path for both.
    ivec2 gsize     = textureSize(g_position_roughness, 0);
    ivec2 origin_px = clamp(ivec2(in_uv * vec2(gsize)), ivec2(0), gsize - 1);

    vec4 norm_met  = texelFetch(g_normal_metallic,    origin_px, 0);
    vec4 pos_rough = texelFetch(g_position_roughness, origin_px, 0);
    vec3 N = norm_met.rgb;
    vec3 P = pos_rough.rgb;
    float roughness = pos_rough.a;

    // Background or high roughness check. Must happen before normalize(N) below --
    // background pixels write N = vec3(0), and normalize(vec3(0)) is NaN, which would
    // silently defeat this early-out (comparisons against NaN are always false in GLSL).
    if (dot(N, N) < 0.001 || roughness >= u_ssr.roughness_cutoff) {
        out_ssr_color = vec4(0.0);
        return;
    }
    N = normalize(N);

    vec3 V = normalize(P - camera.camera_pos);
    vec3 R = reflect(V, N);

    if (dot(R, N) <= 0.0) {
        out_ssr_color = vec4(0.0);
        return;
    }

    GfxSsrParams sp;
    sp.max_distance      = u_ssr.max_distance;
    sp.bias_texels        = u_ssr.bias_texels;
    sp.thickness_min      = u_ssr.thickness_min;
    sp.thickness_scale    = u_ssr.thickness_scale;
    sp.roughness_cutoff   = u_ssr.roughness_cutoff;
    sp.max_iterations     = u_ssr.max_iterations;
    sp.max_hiz_mip        = u_ssr.max_hiz_mip;
    sp.start_mip          = u_ssr.start_mip;
    sp.min_mip0_steps     = u_ssr.min_mip0_steps;
    sp.max_color_mip      = u_ssr.max_color_mip;

    GfxSsrHit hit = gfx_ssr_trace(P, N, roughness, u_ssr.inv_proj, sp);

    // Second, independent trace against the secondary source -- see the push-constant block's
    // doc. Keep whichever hit is physically NEARER along the shared ray (smaller travel); the
    // primary wins ties and the single-hit cases, so this is a verified no-op when
    // has_secondary == 0 (hit_b.hit is always false in that branch's absence).
    if (u_ssr.has_secondary != 0.0) {
        GfxSsrParams sp_b = sp;
        sp_b.max_hiz_mip   = u_ssr.max_hiz_mip_b;
        sp_b.max_color_mip = u_ssr.max_color_mip_b;

        GfxSsrHit hit_b = gfx_ssr_trace_secondary(P, N, roughness, u_ssr.inv_proj, sp_b);
        if (hit_b.hit && (!hit.hit || hit_b.travel < hit.travel)) {
            hit = hit_b;
        }
    }

    out_ssr_color = vec4(hit.color, hit.confidence);
}
