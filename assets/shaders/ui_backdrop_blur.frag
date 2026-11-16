#version 450

// Uniform horizontal-then-vertical blur for the glass UI backdrop (gfxcoopa's
// SeparableBlurPass). Unlike tilt_shift.frag, there is no focus band and no screen-position
// function to evaluate -- every pixel blurs by the same `radius`, so this is tilt_shift.frag
// with the band/ramp machinery removed and `radius` read straight from the push block as the
// FURTHEST tap distance (tilt_shift.frag's max_radius is a per-tap spacing instead, so its
// furthest tap sits at 8x the stated radius -- this shader's radius means what it says).
//
// One shader serves both passes; only pc.texel_step (the tap direction, in UV) differs between
// the two SeparableBlurPass::execute() draw calls, exactly as tilt_shift.frag's single shader
// does for TiltShiftPass.
//
// Runs in sRGB-encoded display space, same as everything else in the overlay composite chain
// (see ui_composite.frag's own doc) -- not a decode/blur/re-encode -- so the result agrees with
// what it sits next to rather than needing its own gamma round trip.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;

layout(push_constant) uniform Push {
    vec2 texel_step;  // blur tap direction: (1/dst_w, 0) or (0, 1/dst_h)
    float radius;     // furthest tap, in destination pixels
    float _pad;
} pc;

const int TAPS = 8; // per side; 17 taps total including the centre

void main() {
    if (pc.radius < 0.05) {
        out_color = texture(u_src, in_uv);
        return;
    }

    vec2 step = pc.texel_step * (pc.radius / float(TAPS));
    vec3 sum = texture(u_src, in_uv).rgb;
    float weight_sum = 1.0;

    for (int i = 1; i <= TAPS; ++i) {
        float x = float(i) / float(TAPS);       // 0..1
        float w = exp(-2.5 * x * x);
        vec2 offset = step * float(i);
        sum += texture(u_src, in_uv + offset).rgb * w;
        sum += texture(u_src, in_uv - offset).rgb * w;
        weight_sum += 2.0 * w;
    }

    out_color = vec4(sum / weight_sum, 1.0);
}
