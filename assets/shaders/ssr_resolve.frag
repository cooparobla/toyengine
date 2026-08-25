#version 450

// Unmodified port of gfxcoopa/assets/shaders/ssr_resolve.frag --
// reprojection/history blending has no GI coupling, so nothing here needed
// to change (see ssr.frag's header comment).

#include <gfx/ssr_common.glsl>

// Temporal resolve for the SSR raymarch output (rgb = confidence-premultiplied hit radiance,
// a = confidence).
//
// Unlike taa.frag -- and unlike this shader's previous revision, which copied taa.frag's
// "no velocity, static reprojection" simplification -- history is now reprojected. G2 already
// stores world position, so all this needs is the previous frame's view-projection.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex_current;            // nearest
layout(set = 0, binding = 1) uniform sampler2D tex_history;            // LINEAR (reprojected UV)
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;   // nearest

layout(push_constant) uniform PushConstants {
    mat4  prev_view_proj;  // previous frame's JITTERED proj * view
    vec2  resolution;      // TRACE resolution (half of the screen under ssr_half_res)
    float blend_factor;
    int   history_valid;   // 0 until both a history image and a previous matrix exist
} pc;

void main() {
    vec4 current = texture(tex_current, in_uv);

    if (pc.history_valid == 0) {
        out_color = current;
        return;
    }

    // Reprojection. G2 holds the world position of the surface this pixel's reflection
    // ORIGINATES from, so this reprojects the reflecting surface, not the reflected image. A
    // reflection parallaxes faster than its host surface -- as a mirror sweeps past a fixed
    // object the virtual image moves at roughly twice the surface's screen velocity -- so this
    // is exact only for points on the mirror plane itself. It removes the bulk of the smear;
    // the residual is what the neighbourhood clamp below is still for. That is why the clamp
    // stays even now that reprojection is real.
    vec3 P = texture(g_position_roughness, in_uv).rgb;
    vec4 prev_clip = pc.prev_view_proj * vec4(P, 1.0);

    // Behind the previous frame's eye: no history exists for this point at all.
    if (prev_clip.w <= 0.0) {
        out_color = current;
        return;
    }

    vec2 prev_uv = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);

    // Off-screen last frame -- the cheapest and most common disocclusion case. CLAMP_TO_EDGE on
    // the history sampler would otherwise smear the border texel inward along every screen edge
    // the camera is turning toward.
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_color = current;
        return;
    }

    // 3x3 neighbourhood AABB of the CURRENT frame's trace. Computed after the early-outs above,
    // which the previous revision did not do.
    vec2 texel_size = 1.0 / pc.resolution;
    vec4 s0 = texture(tex_current, in_uv + vec2(-1.0,  1.0) * texel_size);
    vec4 s1 = texture(tex_current, in_uv + vec2( 0.0,  1.0) * texel_size);
    vec4 s2 = texture(tex_current, in_uv + vec2( 1.0,  1.0) * texel_size);
    vec4 s3 = texture(tex_current, in_uv + vec2(-1.0,  0.0) * texel_size);
    vec4 s4 = current;
    vec4 s5 = texture(tex_current, in_uv + vec2( 1.0,  0.0) * texel_size);
    vec4 s6 = texture(tex_current, in_uv + vec2(-1.0, -1.0) * texel_size);
    vec4 s7 = texture(tex_current, in_uv + vec2( 0.0, -1.0) * texel_size);
    vec4 s8 = texture(tex_current, in_uv + vec2( 1.0, -1.0) * texel_size);

    vec4 aabb_min = min(s0, min(s1, min(s2, min(s3, min(s4, min(s5, min(s6, min(s7, s8))))))));
    vec4 aabb_max = max(s0, max(s1, max(s2, max(s3, max(s4, max(s5, max(s6, max(s7, s8))))))));

    vec4 history = texture(tex_history, prev_uv);

    // Colour: clamp two-sided against the neighbourhood AABB, as before.
    vec3 clamped_rgb = clamp(history.rgb, aabb_min.rgb, aabb_max.rgb);

    // Confidence: clamp against the neighbourhood MAXIMUM only. A two-sided clamp drags an
    // accumulated confidence to zero the instant one neighbour misses, so any surface where the
    // trace alternates hit/miss between frames settles at a flickering half-strength
    // reflection -- the reflection is present but permanently dim, which reads as a bug rather
    // than as "no reflection here". The one-sided clamp still collapses history immediately
    // where the WHOLE neighbourhood missed, which is the disocclusion case it exists for.
    float clamped_a = min(history.a, aabb_max.a);

    out_color = vec4(mix(current.rgb, clamped_rgb, pc.blend_factor),
                     mix(current.a,   clamped_a,   pc.blend_factor));
}
