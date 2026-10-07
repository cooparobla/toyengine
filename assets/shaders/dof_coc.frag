#version 450

// Depth-of-field stage 1/3: CoC + downsample. Reads full-res linear HDR colour and
// gbuffer depth, writes a half-resolution vec4(colour, signed_coc) that stages 2/3
// (dof_bokeh.frag) and 3/3 (dof_composite.frag) both consume. See
// gfxcoopa's dof_pass.h file doc for the three-stage shape and gfx/dof_common.glsl
// for the thin-lens CoC formula.
//
// Downsampling here (rather than gathering at full res) is what keeps the spiral
// gather in dof_bokeh.frag affordable at sample_count up to MAX_DOF_TAPS: every
// tap there is one quarter the pixel count of the source image. The 2x2 box this
// shader averages is exactly the induced footprint of that halving, so no
// information is thrown away that dof_bokeh.frag could have used at full res --
// it only ever samples this half-res image, never the source directly.
//
// Sky pixels (gbuffer.frag's clear value, depth == 1.0 => view_depth == far) need
// no special case: dof_signed_coc() naturally saturates at pc.camera.w (max_radius)
// well before the far plane on any sane focus/aperture setting, so "very far"
// degrades to "as blurred as this pass ever gets", not a NaN or a discontinuity.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color_coc;

layout(set = 0, binding = 0) uniform sampler2D scene_color; // full-res, linear HDR
layout(set = 0, binding = 1) uniform sampler2D scene_depth; // full-res gbuffer depth, NEAREST

#include <gfx/dof_common.glsl>

void main() {

    vec2 texel = pc.inv_size; // full-res texel size; this pass's own dst is half-res

    // The four full-res texels this half-res output texel downsamples.
    vec2 offsets[4] = vec2[4](
        vec2(-0.5, -0.5), vec2(0.5, -0.5),
        vec2(-0.5,  0.5), vec2(0.5,  0.5)
    );

    vec3  color_sum = vec3(0.0);
    float weight_sum = 0.0;
    float max_abs_coc = 0.0;
    float signed_coc_at_max = 0.0;

    for (int i = 0; i < 4; ++i) {
        vec2 uv = in_uv + offsets[i] * texel;
        vec3 c  = texture(scene_color, uv).rgb;
        // Karis average: a soft 1/(1+luma) rolloff on both the weight AND what it
        // weights, same class of per-tap ceiling as BloomPass's clamp_max, but
        // without a hard clamp's visible step against neighbours. NOTE: this is
        // c*w, not (c*w)*w again -- squaring w here was a bug that darkened every
        // downsampled texel by roughly 1/(1+luma) a second time, achromatically
        // (the same w multiplies all three channels), which showed up as DOF's
        // defocused regions reading systematically darker than the sharp source.
        float w = 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
        color_sum  += c * w;
        weight_sum += w;

        float d = texture(scene_depth, uv).r;
        float view_depth = gfx_linear_depth(d, pc.camera.x, pc.camera.y, pc.camera.z);
        float coc = dof_signed_coc(view_depth);
        // Deliberately the MAX |CoC| of the four taps, not their average: a
        // near-field foreground edge's blur footprint must never be under-
        // reported at this downsample, or dof_bokeh.frag's gather would clip the
        // very silhouette bleed the occlusion-aware weighting exists to produce.
        if (abs(coc) > max_abs_coc) {
            max_abs_coc = abs(coc);
            signed_coc_at_max = coc;
        }
    }

    out_color_coc = vec4(color_sum / max(weight_sum, 1e-4), signed_coc_at_max * 0.5);
}
