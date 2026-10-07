#version 450

// contact_shadow_resolve.frag -- temporal accumulation of the contact-shadow march, the scalar
// counterpart of ssr_resolve.frag.
//
// Same push constants, same four bindings and the same converging 1/N schedule against the
// shared TemporalHistoryPass count as ssr_resolve.frag, so ContactShadowPass drives it the same
// way. It drops everything a single occlusion value does not need: the YCoCg conversion and
// three-channel variance clip, and the nine-tap Catmull-Rom history resample (one bilinear tap
// reprojects a smooth scalar mask just as well). With the R16F targets this keeps the pass's
// fixed cost well under the ~1.9 ms the full colour resolve costs at 1080p.

#include <gfx/ssr_common.glsl>

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_occlusion;

layout(set = 0, binding = 0) uniform sampler2D tex_current;   // NEAREST -- this frame's march
layout(set = 0, binding = 1) uniform sampler2D tex_history;   // LINEAR  -- reprojected UV
layout(set = 0, binding = 2) uniform sampler2D u_depth;       // NEAREST -- rasterized scene depth
layout(set = 0, binding = 3) uniform sampler2D u_count;       // TemporalHistoryPass: .g = count

// Byte-for-byte SsrPass::ResolvePushConstants.
layout(push_constant) uniform PushConstants {
    mat4  reproject;       // current clip -> previous clip (double-precision composed on the CPU)
    float resolution_x;
    float resolution_y;
    float max_accum;       // contact_shadow_temporal_frames; 0 = passthrough
    float blend_factor;    // unused: this pass always has a count buffer
    int   history_valid;
    float gamma;           // variance-clip width in standard deviations
    int   frozen;
} pc;

const float kSigmaFloor = 0.02;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float current = texelFetch(tex_current, px, 0).r;
    if (pc.max_accum <= 0.0) {
        out_occlusion = vec4(current, 0.0, 0.0, 0.0);
        return;
    }

    // 3x3 neighbourhood statistics for the variance clip, and its mean as the fallback for a
    // pixel with no usable history.
    ivec2 size = textureSize(tex_current, 0) - 1;
    float m1 = 0.0, m2 = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float s = texelFetch(tex_current, clamp(px + ivec2(x, y), ivec2(0), size), 0).r;
            m1 += s;
            m2 += s * s;
        }
    }
    float mean  = m1 / 9.0;
    float sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    if (pc.history_valid == 0) {
        out_occlusion = vec4(mean, 0.0, 0.0, 0.0);
        return;
    }

    float depth     = texelFetch(u_depth, px, 0).r;
    vec2  ndc       = vec2(in_uv.x * 2.0 - 1.0, -(in_uv.y * 2.0 - 1.0));
    vec4  prev_clip = pc.reproject * vec4(ndc, depth, 1.0);
    if (prev_clip.w <= 0.0) {
        out_occlusion = vec4(mean, 0.0, 0.0, 0.0);
        return;
    }
    vec2 prev_uv = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_occlusion = vec4(mean, 0.0, 0.0, 0.0);
        return;
    }

    // Accumulation count from the shared history-validity buffer: 1 on a disocclusion, rising
    // to the cap while the surface stays put.
    float n = texelFetch(u_count, px, 0).g;
    bool  have_count = n > 0.0;
    n = min(max(n, 1.0), pc.max_accum);

    // Camera and scene still, average topped up: hold verbatim so a resting image is static.
    if (pc.frozen != 0 && have_count && n > 1.0) {
        out_occlusion = vec4(texelFetch(tex_history, px, 0).r, 0.0, 0.0, 0.0);
        return;
    }

    float band    = pc.gamma * sigma + kSigmaFloor;
    float history = clamp(texture(tex_history, prev_uv).r, mean - band, mean + band);
    float w = have_count ? ((n <= 1.0) ? 1.0 : min(1.0 / n, 0.25)) : (1.0 - pc.blend_factor);
    out_occlusion = vec4(mix(history, current, w), 0.0, 0.0, 0.0);
}
