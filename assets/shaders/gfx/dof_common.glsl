#ifndef GFX_DOF_COMMON_GLSL
#define GFX_DOF_COMMON_GLSL

// gfx/dof_common.glsl -- push-constant block, CoC formula and spiral-gather tap
// generator shared by dof_coc.frag/dof_bokeh.frag/dof_composite.frag, matching
// DofPass::PushConstants (dof_pass.h) field-for-field via its own static_assert.
// See gfxcoopa's dof_pass.h file doc for why this is a physically-based thin-lens
// CoC rather than tilt_shift.frag's screen-position-only band: a depth-driven
// field is exactly the case that shader warns a SEPARABLE blur cannot handle
// correctly (silhouette bleeding), which is why this pass gathers instead.

const float GFX_PI = 3.14159265359;
const float GFX_TAU = 6.28318530718;
// Golden angle in radians (2*pi / phi^2) -- successive spiral taps at this angle
// apart tile the disc with near-maximal minimum spacing for any tap count, so a
// fixed generator serves every `sample_count` from 8..MAX_DOF_TAPS without the
// clumping a naive concentric-ring layout would show at low counts.
const float GFX_GOLDEN_ANGLE = 2.39996323;

const int MAX_DOF_TAPS = 48;

// Interleaved gradient noise (Jimenez 2014), as a rotation angle in [0, 2pi).
// Duplicated from gfx/shadow_sampling.glsl's gfx_ign_angle() rather than
// included (that file declares no uniforms/samplers so the include would be
// safe, but this is a post-process pass, not a lighting one -- same
// deliberate-duplication call ssr_common.glsl's ssr_ign() already makes for
// its own copy). Used to rotate dof_spiral_tap()'s per-pixel starting angle;
// see that function's own doc for why an UNROTATED spiral is wrong here.
float dof_ign_angle(vec2 px) {
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))) * GFX_TAU;
}

layout(push_constant) uniform DofPush {
    vec4 lens;     // x: focus_distance_m, y: focal_length_m, z: aperture_diameter_m,
                   // w: coc_px_per_m (full_width / sensor_width_m)
    vec4 camera;   // x: near, y: far, z: is_perspective, w: max_radius (full-res px)
    vec4 bokeh;    // x: sample_count, y: blade_count (< 3 = disc), z: blade_rotation_rad,
                   // w: debug_view (>= 0.5 => true)
    vec2 inv_size; // 1/full_w, 1/full_h
    vec2 focus;    // x: focus_range_m (half-width of the forced-sharp band),
                   // y: blur_scale (|CoC| multiplier, applied before the clamp)
} pc;

#include <gfx/depth.glsl>

// Signed thin-lens circle of confusion, in FULL-res pixels, from a view-space
// distance already in metres.
//   coc_m = A * f * (d - F) / (d * (F - f))
// negative = near field (in front of the focal plane, blurs toward the camera),
// positive = far field. d == F is the exact zero-crossing; d -> F - f (the lens'
// own hyperfocal singularity) is guarded rather than dividing by ~0, since a
// camera can be aimed with focal_length_m >= focus_distance_m during scene setup
// or a bad config value -- this must degrade to "no blur" rather than a NaN/Inf
// that would poison the gather's tap weights.
//
// FOCUS RANGE (pc.focus.x, metres). Everything within that half-width of the focal
// plane is forced to zero CoC. This is the one deliberately NON-physical term here,
// and it exists because the physical sharp band is
//     Delta = c * N * F(F - f) / f^2
// i.e. it collapses with the SQUARE of focus distance: a subject comfortably sharp
// at 11 m has ~8 cm of depth of field at 3 m and is almost entirely defocused. No
// aperture or focal length fixes that -- every dial only rescales a curve that is
// still quadratic in F -- so keeping a SUBJECT sharp across a zoom range needs a
// band whose width does not depend on F at all. DofPass::Params::focus_range's own
// doc covers how toyengine fits it to the focus object's bounds.
//
// The band is applied by SLIDING the depth toward the focal plane by `range`, not by
// widening a threshold. That matters: at |d - F| = range + eps the numerator becomes
// eps, so CoC leaves the band continuously at 0 and the physical falloff outside is
// bit-for-bit the unshifted curve, just re-origined. A thresholded version would step
// from 0 straight to the CoC at `range`, and dof_bokeh.frag/dof_composite.frag's
// shared 1px sharp cutoff and 1->2px blend band both assume CoC is continuous across
// that boundary -- a step there reintroduces exactly the dim ring at every focal-plane
// silhouette that keeping those two thresholds in matching units was meant to remove.
// `range >= 0` (DofPass::execute() floors it) also keeps the slid depth positive, so
// the denominator guard below still covers the degenerate focus <= f case.
float dof_signed_coc(float view_depth_m) {
    float f = pc.lens.y;
    float focus = pc.lens.x;
    float aperture_diameter = pc.lens.z;

    float delta = view_depth_m - focus;
    float range = pc.focus.x;
    if (abs(delta) <= range) return 0.0;
    float d = view_depth_m - sign(delta) * range;

    float denom = d * (focus - f);
    if (abs(denom) < 1e-6) return 0.0;

    float coc_m  = aperture_diameter * f * (d - focus) / denom;
    // blur_scale multiplies BEFORE the clamp, so lowering it actually reduces the blur
    // instead of just moving which pixels sit at the ceiling.
    float coc_px = coc_m * pc.lens.w * pc.focus.y;
    return clamp(coc_px, -pc.camera.w, pc.camera.w);
}

// Uniform-density disc sample i of n (i in [0, n)), unit radius, via the golden-
// angle spiral. r = sqrt((i + 0.5) / n) keeps the innermost tap off the exact
// centre (already covered by the caller's own texel) while still approaching 0
// as i -> 0, and areal density stays uniform tap-to-tap since equal-area annuli
// get equal sample counts under a sqrt radius law.
//
// `rotation` offsets theta by a per-PIXEL angle (the caller passes
// dof_ign_angle(gl_FragCoord.xy)) rather than leaving the spiral's absolute
// orientation fixed. Without it every fragment gathers the exact same n
// offsets in the exact same directions, so wherever the CoC saturates (e.g.
// dof_max_radius, or just a large uniform background) the gather stops
// low-passing the source and instead re-draws one fixed n-tap dot pattern
// screen-wide -- visible as a regular lattice whose spacing tracks n, not
// scene content. Rotating decorrelates that pattern pixel-to-pixel the same
// way gfx_shadow_dir_pcf_vogel's rotation_angle does for shadow PCF (see
// gfx/shadow_sampling.glsl's file doc for the same failure mode on an
// unrotated Vogel spiral).
vec2 dof_spiral_tap(int i, int n, float rotation) {
    float r = sqrt((float(i) + 0.5) / float(n));
    float theta = float(i) * GFX_GOLDEN_ANGLE + rotation;

    int blades = int(pc.bokeh.y);
    if (blades >= 3) {
        // Snap the disc radius onto an N-sided polygon (a physical iris): the
        // regular-polygon "radius at angle" formula, scaled so the polygon's
        // flat-to-flat width still matches the disc's diameter at theta == 0.
        float bf = float(blades);
        float a  = theta + pc.bokeh.z - GFX_PI / bf;
        float wrapped = mod(a, GFX_TAU / bf) - GFX_PI / bf;
        r *= cos(GFX_PI / bf) / cos(wrapped);
    }

    return r * vec2(cos(theta), sin(theta));
}

#endif // GFX_DOF_COMMON_GLSL
