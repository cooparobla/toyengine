#version 450

// Shared history-validity buffer for every temporally-accumulated screen-space effect
// (Unity HDRP's _HistoryValidityBuffer, and the sample counter its denoisers accumulate with).
//
// The question "is the history at this pixel the same surface it was showing last frame?" has
// exactly one answer per pixel per frame, and it depends only on depth and the camera's motion
// -- not on which effect is asking. Answering it once here, and handing every consumer the
// resulting per-pixel SAMPLE COUNT, is what lets SSR, the traced SSGI bounce and the contact
// shadows all run a converging running average (blend the new frame at 1/(n+1)) instead of a
// fixed-rate exponential blend, which can only ever orbit a per-frame-rejittered input.
//
// Output (RG16F, see TemporalHistoryPass::kFormat):
//   R = this pixel's linear view distance. Carried so that NEXT frame's run can tell whether
//       the history it reprojects onto belongs to this same surface.
//   G = accumulation count, 1 on a freshly invalidated pixel, rising to pc.max_accum.
//
// Reprojection starts from the rasterized DEPTH, never from the G-buffer's world position:
// that buffer is RGBA16F, whose quantization step at world coordinates of a few hundred units
// is 0.06-0.25 wu -- reprojected at close range that is a multi-pixel, per-texel-random UV
// error. pc.reproject maps clip space to clip space and never touches a world-scale number.

#include <gfx/depth.glsl>
#include <gfx/ssr_common.glsl>

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_meta;

layout(set = 0, binding = 0) uniform sampler2D u_depth;    // NEAREST -- rasterized scene depth
layout(set = 0, binding = 1) uniform sampler2D u_history;  // LINEAR  -- last frame's own output
// G-buffer velocity (NEAREST), read only when pc.use_velocity != 0: xy = this surface's
// unjittered screen motion since last frame in uv, z = its previous linear view depth (-1 = it
// has no previous pose), w = its current linear view depth. Lets a MOVING object keep its
// history -- the camera-only path below sees its previous-frame pixel as a different surface.
layout(set = 0, binding = 2) uniform sampler2D u_velocity;

layout(push_constant) uniform PushConstants {
    // Current clip space -> previous frame's clip space: prev jittered (proj * view) times the
    // inverse of the current jittered (proj * view), composed in double precision on the CPU
    // (the scheme TaaPass and SsaoPass both use) so no world-scale magnitude and no
    // catastrophic float cancellation ever reaches this shader.
    mat4  reproject;
    // Flattened vec2, so nothing here depends on std430's vec2 base alignment lining up with
    // the C++ struct -- the house rule SsaoPass::PushConstants follows.
    float resolution_x;
    float resolution_y;
    // Accumulation cap. A pixel's count rises by one per accepted frame until it reaches this,
    // which is where a consumer's running average turns into a fixed-rate 1/max_accum blend.
    // 0 makes every pixel read back count 1, i.e. every consumer degenerates to a passthrough
    // of its current frame -- how "temporal accumulation disabled" is expressed.
    float max_accum;
    float near_z;
    float far_z;
    float is_perspective;   // 0 = orthographic; matches gfx_linear_depth()'s own convention
    int   history_valid;    // 0 until both a history image and a previous matrix exist
    int   use_velocity;     // 1: u_velocity is the G-buffer's velocity attachment
} pc;

/// Depth agreement required to accept history, as a fraction of the point's own distance to the
/// previous eye. Relative rather than absolute because the reprojection is exact for static
/// geometry: what this has to absorb is fp16 storage of the distance channel plus the LINEAR
/// history tap blending two depths together, and both scale with the depth itself. Tight enough
/// that a silhouette-crossing tap (where the blend lands between a near and a far surface) is
/// rejected, which is exactly where blended history would be wrong.
const float kDepthTolerance = 0.02;

void main() {
    float depth  = texelFetch(u_depth, ivec2(gl_FragCoord.xy), 0).r;
    float view_z = gfx_linear_depth(depth, pc.near_z, pc.far_z, pc.is_perspective);

    // A fresh pixel starts at 1: the consumer's 1/(n+1) schedule then takes the current frame
    // whole, which is the correct weight for a one-sample average.
    vec4 reset = vec4(view_z, 1.0, 0.0, 0.0);

    if (pc.history_valid == 0 || pc.max_accum <= 0.0) {
        out_meta = reset;
        return;
    }

    vec2  prev_uv;
    float prev_view_z;
    // The far plane (sky) has no rasterized velocity; it takes the camera-only path.
    if (pc.use_velocity != 0 && depth < 1.0) {
        vec4 vel = texelFetch(u_velocity, ivec2(gl_FragCoord.xy), 0);
        // No previous pose (newly spawned, or behind last frame's eye): no history exists.
        if (vel.z <= 0.0) {
            out_meta = reset;
            return;
        }
        // Sub-0.05 px motion is the fp32 noise of re-projecting a static surface, not motion:
        // snapping it to zero keeps a resting pixel's history tap exactly on its own texel.
        vec2 v = vel.xy;
        if (length(v * vec2(pc.resolution_x, pc.resolution_y)) < 0.05) v = vec2(0.0);
        prev_uv     = in_uv - v;
        prev_view_z = vel.z;
    } else {
        vec2 ndc       = vec2(in_uv.x * 2.0 - 1.0, -(in_uv.y * 2.0 - 1.0));
        vec4 prev_clip = pc.reproject * vec4(ndc, depth, 1.0);

        // Behind the previous frame's eye: no history exists for this point at all.
        if (prev_clip.w <= 0.0) {
            out_meta = reset;
            return;
        }
        prev_uv = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);
        // This point's own linear distance along the PREVIOUS view axis -- the same quantity
        // the history's R channel stored for whatever surface that pixel was showing. They
        // match only if that surface was this one.
        prev_view_z = gfx_linear_depth(prev_clip.z / prev_clip.w, pc.near_z, pc.far_z,
                                       pc.is_perspective);
    }

    // Off-screen last frame -- the cheapest and most common disocclusion case, and the one a
    // CLAMP_TO_EDGE history sampler would otherwise smear inward along every screen edge the
    // camera is turning toward.
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_meta = reset;
        return;
    }

    vec2 hist = texture(u_history, prev_uv).rg;
    if (abs(hist.r - prev_view_z) > kDepthTolerance * prev_view_z) {
        out_meta = reset;
        return;
    }

    out_meta = vec4(view_z, min(hist.g + 1.0, pc.max_accum), 0.0, 0.0);
}
