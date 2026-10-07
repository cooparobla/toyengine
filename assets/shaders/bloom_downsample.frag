#version 450

// Bloom stage 2 of 3: one progressive halving step of the bloom pyramid.
//
// Same 13-tap Jimenez / COD AW kernel as bloom_prefilter.frag, minus the threshold
// and the Karis average -- both belong to the first level only (see that file).
// Deliberately NOT scene_color_downsample.frag's texelFetch 2x2 box: integer
// texel indices give adjacent outputs disjoint, non-overlapping support, so which
// four texels land in a box flips discretely under sub-texel camera motion and the
// result pops frame to frame. Every tap here is a continuous `texture()` fetch
// through a VK_FILTER_LINEAR sampler, and the 4x4 footprint overlaps its
// neighbours, so sub-texel motion moves the output smoothly.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;  // the previous (2x larger) level

layout(push_constant) uniform BloomDownsamplePush {
    vec2 src_texel;   // 1.0 / SOURCE size -- taken from the source target's own
                      // width()/height(), not from doubling the destination size:
                      // integer >> halving truncates (45 -> 22), so the two are not
                      // the same number and the offsets must match the texture
                      // actually being sampled.
} pc;

vec3 tap(vec2 offset) { return texture(u_src, in_uv + offset * pc.src_texel).rgb; }

void main() {
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

    // Centre 2x2 group weighted 0.5, four corner groups 0.125 each; weights sum to
    // exactly 1.0 (0.5 + 4 * 0.125), so the pyramid is energy preserving.
    vec3 result = (d + e + i + j) * 0.125
                + (a + b + g + f) * 0.03125
                + (b + c + h + g) * 0.03125
                + (f + g + l + k) * 0.03125
                + (g + h + m + l) * 0.03125;

    out_color = vec4(result, 1.0);
}
