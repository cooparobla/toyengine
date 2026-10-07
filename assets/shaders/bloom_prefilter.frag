#version 450

// Bloom stage 1 of 3: bright-pass extraction fused with the first (full-res ->
// half-res) downsample of the pyramid. See gfxcoopa's BloomPass.
//
// Fused, not two passes, for two reasons: the 13-tap downsample kernel would
// otherwise run twice over the largest surface in the pyramid, and thresholding
// must happen per TAP (before any averaging) or a single very bright texel
// surrounded by dark ones averages below the threshold and never blooms (see
// BloomPass's doc).
//
// Threshold uses Unity's quadratic soft-knee response (PostProcessing v2's
// QuadraticThreshold): a hard `max(br - threshold, 0)` cutoff is C0-discontinuous
// in brightness, so a texel drifting across the threshold as the camera moves pops
// on and off between frames. The knee makes the response C1, so sub-frame
// brightness changes produce sub-frame bloom changes.
//
// The 13-tap kernel is the Jimenez / "Next Generation Post Processing in Call of
// Duty: Advanced Warfare" downsample: five overlapping 2x2 bilinear groups spanning
// a 4x4 source footprint. Overlap is the temporal-stability property -- each source
// texel feeds several destination texels with continuous weights, so sub-texel
// motion interpolates instead of snapping the way a rigid non-overlapping
// texelFetch box grid does.
//
// The per-group Karis average (weight by 1/(1+luma)) runs on THIS stage only, again
// following COD AW: it is what suppresses fireflies (isolated ultra-bright texels),
// which no amount of kernel smoothness fixes. It is energy-biased (it under-weights
// genuinely bright large areas), which is why it is confined to the first level.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;  // full-res linear HDR scene colour

layout(push_constant) uniform BloomPrefilterPush {
    vec2  src_texel;   // 1.0 / source size, in source texels
    float threshold;   // brightness (max3 of RGB) below which nothing glows
    float soft_knee;   // 0..1 fraction of `threshold` the quadratic knee spans
    float clamp_max;   // hard ceiling on any single tap, pre-threshold (firefly guard)
} pc;

float brightness(vec3 c) { return max(c.r, max(c.g, c.b)); }
float luma(vec3 c)       { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Unity's quadratic soft-knee bright pass (PostProcessing v2 QuadraticThreshold).
vec3 prefilter(vec3 c) {
    c = min(c, vec3(pc.clamp_max));
    float br = brightness(c);
    // knee floored above zero: soft_knee == 0 must degrade to a hard cutoff, not a
    // division spike.
    float knee = max(pc.threshold * pc.soft_knee, 1e-4);
    float soft = br - pc.threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(soft, br - pc.threshold);
    return c * (contribution / max(br, 1e-5));
}

vec3 tap(vec2 offset) {
    return prefilter(texture(u_src, in_uv + offset * pc.src_texel).rgb);
}

// One Karis weight per 2x2 group, not per tap: weighting individual taps would
// break the group's own bilinear coherence.
float karis(vec3 c) { return 1.0 / (1.0 + luma(c)); }

void main() {
    // 4x4 footprint, 13 taps. Letters follow the COD AW slide's naming.
    //   a . b . c
    //   . d . e .
    //   f . g . h
    //   . i . j .
    //   k . l . m
    vec3 a = tap(vec2(-2.0, -2.0));
    vec3 b = tap(vec2( 0.0, -2.0));
    vec3 c = tap(vec2( 2.0, -2.0));
    vec3 d = tap(vec2(-1.0, -1.0));
    vec3 e = tap(vec2( 1.0, -1.0));
    vec3 f = tap(vec2(-2.0,  0.0));
    vec3 g = tap(vec2( 0.0,  0.0));
    vec3 h = tap(vec2( 2.0,  0.0));
    vec3 i = tap(vec2(-1.0,  1.0));
    vec3 j = tap(vec2( 1.0,  1.0));
    vec3 k = tap(vec2(-2.0,  2.0));
    vec3 l = tap(vec2( 0.0,  2.0));
    vec3 m = tap(vec2( 2.0,  2.0));

    // Five 2x2 groups: one centre (kernel weight 0.5) + four corners (0.125 each).
    vec3 g0 = (d + e + i + j) * 0.25;
    vec3 g1 = (a + b + g + f) * 0.25;
    vec3 g2 = (b + c + h + g) * 0.25;
    vec3 g3 = (f + g + l + k) * 0.25;
    vec3 g4 = (g + h + m + l) * 0.25;

    float w0 = karis(g0) * 0.500;
    float w1 = karis(g1) * 0.125;
    float w2 = karis(g2) * 0.125;
    float w3 = karis(g3) * 0.125;
    float w4 = karis(g4) * 0.125;

    vec3 result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4)
                / max(w0 + w1 + w2 + w3 + w4, 1e-5);

    out_color = vec4(result, 1.0);
}
