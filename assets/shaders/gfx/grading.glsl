#ifndef GFX_GRADING_GLSL
#define GFX_GRADING_GLSL

// gfx/grading.glsl -- colour-grading strip LUT lookup.
//
// Reconstructs a trilinear 3D cube lookup from a 2D strip of N slices (see
// GradingLut's file doc for the layout: N*N wide by N tall, blue selecting the
// slice). The hardware's bilinear filter supplies the red and green
// interpolation; only the blue axis is lerped here, between two slice taps.
//
// Applied to DISPLAY-REFERRED colour (post-tonemap, post-sRGB-encode), which is
// what a LUT authored in an image editor or a grading tool expects to receive --
// feeding it linear HDR would grade through a completely different curve.

/// Grades `color` (in [0,1], display-referred) through an N-cube strip LUT.
/// `size` is the cube side length N; <= 1 returns the colour unchanged, so a
/// missing LUT is a no-op rather than a black frame.
vec3 gfx_apply_grading_lut(sampler2D lut, vec3 color, float size) {
    if (size <= 1.0) return color;

    color = clamp(color, 0.0, 1.0);

    float slice_width = 1.0 / size;       // one slice's share of the strip's width
    float max_index   = size - 1.0;

    // Texel-centre coordinates WITHIN a slice. Staying inside [0.5, size-0.5]
    // texels is what keeps the hardware's bilinear tap from bleeding across the
    // slice boundary into an unrelated blue level.
    float u = (color.r * max_index + 0.5) / (size * size);
    float v = (color.g * max_index + 0.5) / size;

    float blue = color.b * max_index;
    float s0   = floor(blue);
    float s1   = min(s0 + 1.0, max_index);

    vec3 c0 = texture(lut, vec2(u + s0 * slice_width, v)).rgb;
    vec3 c1 = texture(lut, vec2(u + s1 * slice_width, v)).rgb;
    return mix(c0, c1, blue - s0);
}

#endif // GFX_GRADING_GLSL
