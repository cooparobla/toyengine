#ifndef GFX_VOLUMETRICS_GLSL
#define GFX_VOLUMETRICS_GLSL

// gfx/volumetrics.glsl -- density field for raymarched LOCAL volumes, used by
// volumetrics_march.frag (see engine/passes/volumetrics_pass.h).
//
// Declares no uniforms, samplers, or blocks -- same rule as gfx/fog.glsl and
// gfx/ssr_common.glsl: every input is a function parameter.
//
// The counterpart to gfx/fog.glsl, and the contrast is the point. Fog is the
// GLOBAL atmosphere: a medium everywhere, so its optical depth has a closed form
// and fog.frag never marches. Everything here is a BOUNDED volume whose density
// varies within it -- which has no closed form, so volumetrics_march.frag marches.
//
// One function serves three looks, selected by `kind`:
//
//   FOG   uniform density inside the bounds. A static soft pocket; the
//         containment weight does all the shaping.
//   WIND  ridged noise -> thin ribbons. Smooth fbm's maxima are rounded lumps and
//         stretching a lump only makes a longer lump; ridged noise puts the maxima
//         on sheets, which stretch into ribbons. See gfx_ridged_fbm_3d.
//   HAZE  smooth fbm -> drifting billows. A continuous medium whose density
//         varies, which is why it must NOT take the sparsity gate -- a threshold
//         would break it back into the disconnected blobs ribbons replaced.
//
// Shared by all three: advection (the field slides through the world so features
// translate coherently instead of boiling), an optional low-frequency domain warp
// so they meander rather than running straight, an anisotropic stretch along the
// direction, and an exponential height falloff.

#include <gfx/noise.glsl>

#define GFX_VOLUME_KIND_FOG  0
#define GFX_VOLUME_KIND_WIND 1
#define GFX_VOLUME_KIND_HAZE 2

/// Interleaved gradient noise in [0,1), for dithering a raymarch's start offset.
///
/// Duplicated rather than included from gfx/ssr_common.glsl, which declares a pile
/// of SSR-specific helpers this pass has no use for -- the same trade
/// gfx/dof_common.glsl already makes duplicating gfx_ign_angle.
///
/// @param px    Pixel coordinate (gl_FragCoord.xy).
/// @param frame Frame index; shifts the pattern by an R2 low-discrepancy step so a
///              temporal accumulator (TAA) averages DIFFERENT dither patterns
///              instead of re-resolving one frozen one. Matters most for thin
///              ribbons, which are only a couple of steps across.
float gfx_volume_ign(vec2 px, int frame) {
    px += 5.588238 * float(frame & 63);
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

/**
 * @brief NORMALISED density field of one volume at a world point, in [0,1].
 *
 * Returns the field SHAPE only -- the caller multiplies by the volume's density
 * and its containment weight. Keeping those out means one volume's bounds can
 * soften its own field without the field function knowing anything about bounds.
 *
 * @param p         World-space sample point (engine is Z-up).
 * @param time      Seconds since start; drives advection.
 * @param kind      GFX_VOLUME_KIND_FOG / _WIND / _HAZE.
 * @param dir_speed xyz = normalized advection direction, w = speed (world units/sec).
 * @param field     x = noise scale, y = streak, z = coverage, w = fbm gain.
 * @param shape     x = UNUSED here (the caller applies density), y = height base,
 *                  z = height falloff (<= 0 disables), w = fbm octave count.
 * @param flow      x = meander amplitude (world units, 0 disables), y = meander
 *                  frequency, z = ridge sharpness, w = gate frequency (<= 0 disables).
 * @return Coverage in [0,1]; 0 in the clear space between features.
 */
float gfx_volume_field(vec3 p, float time, int kind,
                       vec4 dir_speed, vec4 field, vec4 shape, vec4 flow) {
    // A fog volume is uniform inside its bounds -- there is no field to evaluate,
    // and returning early skips every noise tap below. This is what makes a static
    // pocket nearly free compared to the animated kinds.
    if (kind == GFX_VOLUME_KIND_FOG) {
        return 1.0;
    }

    vec3 dir = dir_speed.xyz;
    vec3 q   = p - dir * (dir_speed.w * time);

    // Meander. Applied BEFORE the anisotropic stretch, deliberately: warping
    // afterwards would put the displacement through the same compression along the
    // direction axis as everything else, flattening the snake out on the one axis
    // you actually want to see it on.
    if (flow.x > 0.0) {
        vec3 wp = q * flow.y;
        vec3 warp = gfx_value_noise3_3d(wp) * 2.0 - 1.0;
        q += warp * flow.x;
    }

    // Anisotropic stretch. Decomposed into along/perp components against `dir`
    // rather than applied as a diagonal scale (`q / vec3(streak, 1, 1)`), which
    // would only elongate along +X and silently do nothing for any other direction.
    float along = dot(q, dir);
    vec3  perp  = q - along * dir;
    vec3  sp    = (perp + dir * (along / max(field.y, 1e-3))) * field.x;

    float n = (kind == GFX_VOLUME_KIND_WIND)
            ? gfx_ridged_fbm_3d(sp, int(shape.w), 2.0, field.w, flow.z)
            : gfx_fbm_3d(sp, int(shape.w), 2.0, field.w);

    // Sparsity gate, only when asked for. Ridges tile space uniformly, so ribbons
    // need it or the volume fills edge to edge and reads as texture. Haze must not
    // have it: a continuous medium thresholded into isolated peaks stops being a
    // medium. Sampled in the ALREADY-STRETCHED space so gaps elongate too, giving
    // long unbroken runs rather than chopped-up clumps.
    float w = (flow.w > 0.0)
            ? n * smoothstep(field.z, 1.0, gfx_value_noise_3d(sp * flow.w))
            : smoothstep(field.z, 1.0, n);

    // Exponential height falloff. max(p.z - base, 0.0), NOT fog's unclamped
    // exp(-(z - base)/k): fog tolerates density growing without bound below the
    // base because it integrates in closed form over a bounded segment, but a
    // per-sample march would turn that growth into exploding density underground.
    float h = (shape.z > 0.0) ? exp(-max(p.z - shape.y, 0.0) / shape.z) : 1.0;

    return w * h;
}

#endif // GFX_VOLUMETRICS_GLSL
