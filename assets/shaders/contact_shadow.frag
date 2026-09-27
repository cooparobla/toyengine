#version 450

// contact_shadow.frag -- the screen-space contact-shadow march, as its own full-screen pass.
//
// Computes exactly the term pixel_lighting.frag used to evaluate inline, by including the same
// contact_shadow_body.glsl. Splitting it out buys two things the inline version could not have:
//
//  1. A BUFFER to filter. The march is a short stochastic-ish trace over G-buffer texels, and
//     inline it had nowhere to accumulate -- its only temporal filter was the whole-frame TAA,
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
// Set layout matches pixel_lighting.frag's first two sets plus its G-buffer set, so the march
// resolves the identical `camera` / `lights` / `g_*` names its include contract requires.

#include <gfx/spot_light.glsl>
#include <gfx/ssr_common.glsl>

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_occlusion;

// Set 0: Camera UBO -- identical layout to pixel_lighting.frag's.
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

// Must follow the declarations above, not precede them -- the same required-before-include
// contract gfx/ssr_trace_body.glsl documents for itself.
#include "contact_shadow_body.glsl"

void main() {
    // texelFetch through the NEAREST sampler, matching every other G-buffer point-lookup in
    // this engine: the G-buffer is discrete per-pixel data and must never be blended across a
    // silhouette. (Measured: at 1:1 render resolution a LINEAR `texture()` at these
    // texel-centred UVs returns the identical value and differentiates identically, so this is
    // about intent and about not depending on the two resolutions staying equal.)
    ivec2 gsize    = textureSize(g_position_roughness, 0);
    ivec2 px       = clamp(ivec2(in_uv * vec2(gsize)), ivec2(0), gsize - 1);
    vec3  world_pos = texelFetch(g_position_roughness, px, 0).rgb;
    vec3  N_raw     = texelFetch(g_normal_metallic, px, 0).rgb;

    // Evaluated for EVERY fragment, before the background branch below: derivatives are
    // undefined under non-uniform control flow, and a branch taken by only part of the 2x2 quad
    // is exactly that. This is the other half of the same contract.
    vec3 dpdx = dFdx(world_pos);
    vec3 dpdy = dFdy(world_pos);

    // Background pixels write N = vec3(0), and normalize(vec3(0)) is NaN -- check before
    // normalizing, the same guard ssr.frag and ssao.frag use at their own early-outs.
    if (dot(N_raw, N_raw) < 0.001) {
        out_occlusion = vec4(0.0);
        return;
    }
    vec3 N = normalize(N_raw);

    vec3 V = normalize(camera.camera_pos - world_pos);
    vec3 L = normalize(-lights.dir_direction.xyz);

    float occlusion = toy_contact_shadow(world_pos, N, V, L, dpdx, dpdy);

    // Left in .r alone, NOT replicated across rgb. Replicating looks tidier -- a grey triple
    // maps to (x, 0, 0) in the YCoCg space the shared resolve clips in, so the chroma clips
    // become exact no-ops -- but that is precisely why it is worse here: the value then rides on
    // a single clipped channel instead of three, and three independent clips bound the
    // reconstructed scalar more tightly than one. The tighter bound is what rejects the march's
    // spurious far-field hits, which are the white speckles this term is prone to at distance.
    out_occlusion = vec4(occlusion, 0.0, 0.0, 0.0);
}
