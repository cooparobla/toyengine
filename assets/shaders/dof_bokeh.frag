#version 450

// Depth-of-field stage 2/3: golden-angle spiral gather over dof_coc.frag's
// half-res (colour, signed CoC) output. See gfx/dof_common.glsl for the tap
// generator (and its optional N-blade iris snap) and gfxcoopa's dof_pass.h file
// doc for why this pass exists instead of a separable Gaussian: the CoC here is
// depth-driven, so it has hard discontinuities at silhouette edges that a
// horizontal-then-vertical blur (tilt_shift.frag's approach) would leak across.
// A single gather pass has no such ordering to leak across -- every tap either
// contributes or doesn't, independent of every other tap.
//
// NEAR-FIELD DILATION. The per-tap occlusion weight below is what lets a blurred
// FOREGROUND bleed over a sharp background, but it only ever runs for pixels this
// shader actually gathers. Deciding that by the CENTRE texel's own CoC alone
// would make an in-focus pixel early-out so no foreground could ever reach it:
// a defocused foreground with a razor-sharp silhouette, blurred on the inside and
// hard-cut against whatever was behind it. So this pass scans a disc of radius max_radius (the largest |CoC| this pass can produce, so nothing
// that could reach this pixel lies outside it) for the most NEGATIVE neighbouring
// CoC, and gathering at that radius when it exceeds the centre's own.
//
// Only the near (negative) side is dilated. A far-field surface must NOT bleed
// forward over a sharp foreground -- it is occluded by it -- and that asymmetry is
// exactly what the per-tap weight already encodes; dilating both signs would undo it.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_bokeh_coc;

layout(set = 0, binding = 0) uniform sampler2D u_coc; // dof_coc.frag's output, half-res, LINEAR

#include <gfx/dof_common.glsl>

void main() {
    vec4  center = texture(u_coc, in_uv);
    float coc    = center.a; // half-res pixels, signed

    vec2 half_texel = pc.inv_size * 2.0; // this pass's own resolution is half of pc.inv_size's
    // Per-pixel spiral rotation -- see dof_spiral_tap()'s own doc for why an
    // unrotated spiral shows up as a fixed dot lattice wherever CoC saturates.
    // Shared by the dilation scan and the gather so neither can lay down a fixed
    // screen-locked pattern of its own.
    float rotation = dof_ign_angle(gl_FragCoord.xy);

    // --- Near-field dilation (see the file doc) ---
    // pc.camera.w is max_radius in FULL-res px; halve it for this pass's own
    // half-res grid. DILATE_TAPS is a coarse estimator, not a gather: it only has
    // to notice that SOME defocused foreground is within reach, and the gather
    // below then re-samples that neighbourhood properly with the occlusion weight.
    const int DILATE_TAPS = 16;
    float search_px = pc.camera.w * 0.5;
    float near_coc  = min(0.0, coc); // signed, most-negative wins
    for (int i = 0; i < DILATE_TAPS; ++i) {
        vec2 o_px = dof_spiral_tap(i, DILATE_TAPS, rotation) * search_px;
        near_coc  = min(near_coc, texture(u_coc, in_uv + o_px * half_texel).a);
    }

    // The radius this pixel actually gathers over: its own CoC, or a neighbouring
    // near-field one that reaches it, whichever is larger. Widening the radius can
    // only ADD candidate taps -- the per-tap weight below still rejects every tap
    // whose own CoC does not span the offset -- so this never drags a sharp
    // neighbour into an in-focus pixel.
    float radius = max(abs(coc), -near_coc);

    // Signed CoC this pass is effectively working at, written to .a for
    // dof_composite.frag to blend from (it cannot recompute this: a dilated near
    // CoC is a property of the NEIGHBOURHOOD, not of this pixel's own depth).
    float effective_coc = abs(coc) >= radius ? coc : near_coc;

    // Sharp/near-sharp texel with nothing defocused reaching it: a single tap is
    // exact (nothing within 1px of centre can be resolved as blur anyway), and
    // this is the common case across most of a typical frame -- skip the loop
    // entirely rather than spending sample_count taps converging on the same
    // answer. Threshold is 0.5 HALF-res px, i.e. 1 FULL-res px -- must match
    // dof_composite.frag's own `abs(coc) < 1.0` full-res early-out exactly, or the
    // two disagree on which band is "still sharp" and a dim seam/ring appears at
    // the mismatch. Both sides test the DILATED value (this one directly, that
    // one via .a below), so they agree pixel for pixel.
    if (radius < 0.5) {
        out_bokeh_coc = vec4(center.rgb, effective_coc);
        return;
    }

    int n = clamp(int(pc.bokeh.x), 8, MAX_DOF_TAPS);

    vec3  sum  = center.rgb;
    float wsum = 1.0;

    for (int i = 0; i < n; ++i) {
        vec2 o_unit = dof_spiral_tap(i, n, rotation);
        vec2 o_px   = o_unit * radius;
        vec4 tap    = texture(u_coc, in_uv + o_px * half_texel);

        // Occlusion-aware weight: a tap contributes only if ITS OWN circle of
        // confusion reaches out far enough to cover this pixel (length(o_px) away).
        // This is the asymmetry a depth-blind or separable blur cannot express: a
        // blurred foreground tap (large |tap.a|) bleeds over a sharp background
        // centre, but a sharp foreground tap (small |tap.a|) does NOT get pulled
        // into a blurred background's gather, because it fails this test at any
        // offset beyond its own tiny CoC.
        float w = clamp(abs(tap.a) - length(o_px) + 1.0, 0.0, 1.0);
        sum  += tap.rgb * w;
        wsum += w;
    }

    out_bokeh_coc = vec4(sum / max(wsum, 1e-4), effective_coc);
}
