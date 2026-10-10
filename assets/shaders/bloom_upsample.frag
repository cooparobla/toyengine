#version 450

// Bloom stage 3 of 3: 9-tap tent upsample of the next-coarser level, combined with
// the same-resolution downsample-chain level.
//
// The combine is a fragment-shader operation on two bound samplers, not a hardware
// blend into an existing target: pipeline::RenderPass hardcodes LOAD_OP_CLEAR
// (gfxcoopa/pipeline/render_pass.h), so no target can be reopened and composited
// onto in place, and BlendMode::Additive would only help across multiple draws
// inside ONE open pass -- which this pyramid never has. Same reasoning FogPass and
// StylizePass document for their own multi-input composites.
//
// mix() with `scatter`, not a bare sum: the resulting per-level weights form the
// geometric series (1-s), s(1-s), s^2(1-s), ... s^(N-1), which sums to exactly 1.
// The pyramid therefore stays energy-normalised and bloom_intensity stays a single
// meaningful scalar whose meaning does not shift if the level count changes.
// scatter = 0.7 matches Unity URP's Bloom default.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_low;   // next-coarser level (half this size)
layout(set = 0, binding = 1) uniform sampler2D u_high;  // same-resolution downsample level

layout(push_constant) uniform BloomUpsamplePush {
    vec2  low_texel;  // 1.0 / u_low's size: the tent spans +/- one COARSE texel
    float radius;     // tent width multiplier; 1.0 = the standard kernel
    float scatter;    // 0 = keep only u_high, 1 = keep only the tent of u_low
} pc;

// 3x3 tent (bilinear-triangle) filter -- the standard Kawase / dual-filter upsample.
// Weights 1,2,1 / 2,4,2 / 1,2,1 over 16. Every tap is a continuous `texture()` read,
// and the kernel's support overlaps that of neighbouring destination texels, which
// is what makes the reconstruction smooth under sub-pixel motion rather than
// re-introducing blockiness at each doubling.
vec3 upsample_tent(vec2 uv) {
    vec2 o = pc.low_texel * pc.radius;
    vec3 s = vec3(0.0);
    s += texture(u_low, uv + vec2(-o.x, -o.y)).rgb * 1.0;
    s += texture(u_low, uv + vec2( 0.0, -o.y)).rgb * 2.0;
    s += texture(u_low, uv + vec2( o.x, -o.y)).rgb * 1.0;
    s += texture(u_low, uv + vec2(-o.x,  0.0)).rgb * 2.0;
    s += texture(u_low, uv                   ).rgb * 4.0;
    s += texture(u_low, uv + vec2( o.x,  0.0)).rgb * 2.0;
    s += texture(u_low, uv + vec2(-o.x,  o.y)).rgb * 1.0;
    s += texture(u_low, uv + vec2( 0.0,  o.y)).rgb * 2.0;
    s += texture(u_low, uv + vec2( o.x,  o.y)).rgb * 1.0;
    return s * (1.0 / 16.0);
}

void main() {
    vec3 high = texture(u_high, in_uv).rgb;
    vec3 low  = upsample_tent(in_uv);
    out_color = vec4(mix(high, low, pc.scatter), 1.0);
}
