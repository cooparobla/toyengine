#version 450

// Tilt-shift diorama blur (Zelda: Link's Awakening [Switch] reference): a horizontal
// sharp band with the top and bottom progressively blurred, simulating a macro lens
// with a shifted focal plane. See gfxcoopa's TiltShiftPass.
//
// The circle of confusion (CoC) is a pure function of screen position -- no depth is
// sampled -- so the field is perfectly smooth everywhere, including across silhouette
// edges. That is what makes the separable two-pass Gaussian below exact: a
// depth-driven CoC would have hard discontinuities at foreground/background edges that
// a horizontal-then-vertical blur leaks across (sharp foreground bleeding into blurred
// background, or vice versa). One shader serves both passes; only pc.step (the tap
// direction, in UV) differs between the two TiltShiftPass::execute() draw calls.
//
// Deliberately runs at DISPLAY resolution (see TiltShiftPass's own file doc for why),
// so u_src is the low-res post_target_ and this shader also performs the nearest-
// upscale: on the horizontal pass, texture(u_src, in_uv) with a NEAREST sampler at a
// destination-resolution UV is bit-identical to upscale.frag's own output. Combined
// with the coc < EPS early-out below, this guarantees the fully-sharp band matches the
// plain nearest-upscaled image exactly, tap for tap.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;

layout(push_constant) uniform TiltShiftPush {
    vec4 band;        // x: focus_center (0..1, screen-space along axis)
                       // y: focus_width  (half-height of the fully-sharp band)
                       // z: ramp_width   (distance the blur ramps in over)
                       // w: max_radius   (destination-pixel radius at full strength)
    vec4 shape;        // x: blur_top strength, y: blur_bottom strength
                       // zw: rotated screen axis (unit vector; .y component grows
                       //     "downward" the same way in_uv.y does at angle == 0)
    vec2 texel_step;   // blur tap direction: (1/dst_w, 0) or (0, 1/dst_h), scaled by CoC
} pc;

const int TAPS = 8; // per side; 17 taps total including the centre

// Signed distance from the focus band centre, along the (possibly rotated) axis;
// reduces to (in_uv.y - focus_center) when shape.zw == (0, 1) (angle == 0).
float band_side() {
    vec2 p = in_uv - 0.5;
    float band = dot(p, pc.shape.zw) + 0.5;
    return band - pc.band.x;
}

// 0 inside the sharp band, ramping smoothly to 1 over ramp_width, scaled by the
// top/bottom strength and max_radius. Smoothstepped (not linear) so the transition
// has no visible slope discontinuity at either edge of the ramp.
float circle_of_confusion(float side) {
    float t = clamp((abs(side) - pc.band.y) / max(pc.band.z, 1e-4), 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t);
    float strength = side < 0.0 ? pc.shape.x : pc.shape.y;
    return t * strength * pc.band.w;
}

void main() {
    float coc = circle_of_confusion(band_side());

    // Sharp band (and anywhere strength/radius is zeroed out): a single tap, so this
    // path is exactly the nearest-upscale it replaces -- see the file doc above.
    if (coc < 0.05) {
        out_color = texture(u_src, in_uv);
        return;
    }

    vec2 step = pc.texel_step * coc;
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
