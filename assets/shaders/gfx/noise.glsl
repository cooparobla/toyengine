#ifndef GFX_NOISE_GLSL
#define GFX_NOISE_GLSL

// gfx/noise.glsl -- procedural 3D value noise + fbm, for volumetric density
// fields (see gfx/volumetrics.glsl and volumetrics_march.frag).
//
// Declares no uniforms, samplers, or blocks -- same rule as gfx/fog.glsl and
// gfx/ssr_common.glsl: every input is a function parameter, since different
// passes bind their sets at different indices.
//
// Procedural rather than a sampled 3D texture because gfxcoopa has no 3D
// texture support at all (no VK_IMAGE_TYPE_3D anywhere), and adding it for one
// effect would be a far larger change than the ~10 ALU ops per tap this costs.

/// 3D hash -> [0,1). Deliberately sine-free (Dave Hoskins' hash13) rather than
/// the common `fract(sin(dot(p, ...)) * k)` dither-hash idiom: that
/// form's precision collapses at large arguments, and a WORLD-space volumetric
/// sample reaches coordinates orders of magnitude larger than the screen-space
/// pixel coordinates a dither hash is ever fed.
float gfx_hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

/// Trilinear value noise in [0,1], smoothstep-interpolated.
///
/// Value noise rather than gradient/simplex noise on purpose: a raymarched
/// density field evaluates this `steps * octaves` times per pixel, so the
/// per-tap constant factor dominates the visual difference. Value noise costs
/// 8 hashes + 7 mixes; gradient noise adds 8 dot products and gradient
/// construction on top of the same 8 hashes. Value noise's axis-aligned grid
/// bias is invisible once fbm'd, advected, and pushed through a sparsity
/// threshold (see gfx_volume_field) -- none of which preserve grid structure.
///
/// C1 continuity (the classic `f*f*(3-2f)` smoothstep) is sufficient: the
/// march samples the field, it never differentiates it, so the C2 quintic
/// Perlin uses to keep its derivative continuous would buy nothing here.
float gfx_value_noise_3d(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(mix(mix(gfx_hash13(i + vec3(0.0, 0.0, 0.0)),
                       gfx_hash13(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(gfx_hash13(i + vec3(0.0, 1.0, 0.0)),
                       gfx_hash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(gfx_hash13(i + vec3(0.0, 0.0, 1.0)),
                       gfx_hash13(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(gfx_hash13(i + vec3(0.0, 1.0, 1.0)),
                       gfx_hash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

/// 3D hash -> three independent values in [0,1)^3 (Dave Hoskins' hash33, the
/// vector sibling of gfx_hash13 above, with the same sine-free precision argument).
vec3 gfx_hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

/// Three independent value-noise channels in [0,1]^3 from ONE lattice walk.
///
/// For a domain warp, which needs a random VECTOR per point: three scalar
/// gfx_value_noise_3d() calls at offset positions cost 24 hashes and three
/// cell setups; this shares the cell and draws all three channels from each
/// corner's single gfx_hash33 -- 8 hashes, about a third of the cost, for a
/// field with the same per-channel statistics.
vec3 gfx_value_noise3_3d(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);

    return mix(mix(mix(gfx_hash33(i + vec3(0.0, 0.0, 0.0)),
                       gfx_hash33(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(gfx_hash33(i + vec3(0.0, 1.0, 0.0)),
                       gfx_hash33(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(gfx_hash33(i + vec3(0.0, 0.0, 1.0)),
                       gfx_hash33(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(gfx_hash33(i + vec3(0.0, 1.0, 1.0)),
                       gfx_hash33(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

/// Amplitude-NORMALIZED fbm in [0,1].
///
/// Normalized (dividing by the summed amplitude) rather than returning the raw
/// sum, so a caller's threshold against this value means the same thing at any
/// octave count. Without it, raising the octave count would silently shift the
/// output's range and retune every sparsity/coverage knob downstream -- the
/// class of "a threshold quietly turns the whole effect into a no-op" bug this
/// engine has already been bitten by once (see SsaoPass's bias units).
///
/// @param p          Sample point (pre-scaled by the caller).
/// @param octaves    Number of fbm octaves; 1 is a single smooth noise layer.
/// @param lacunarity Frequency multiplier per octave (2.0 is conventional).
/// @param gain       Amplitude multiplier per octave (0.5 is conventional).
float gfx_fbm_3d(vec3 p, int octaves, float lacunarity, float gain) {
    float sum  = 0.0;
    float amp  = 1.0;
    float norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * gfx_value_noise_3d(p);
        norm += amp;
        p    *= lacunarity;
        amp  *= gain;
    }
    return sum / max(norm, 1e-5);
}

/// Ridged fbm in [0,1]: folds each octave about its midpoint so the maxima land
/// on the noise's zero-crossing SURFACES rather than on its isolated peaks.
///
/// This is the difference between a blobby field and a filament field, and no
/// amount of thresholding or stretching gets you there from gfx_fbm_3d: smooth
/// fbm has ROUNDED maxima, so its high regions are lumps, and an anisotropic
/// stretch only ever turns a lump into a longer lump. Folding moves the maxima
/// onto continuous sheets, which a stretch turns into ribbons.
///
/// `sharpness` powers the crest -- 1 is a soft ridge, 6+ a thin strand. Applied
/// PER OCTAVE rather than to the summed result, so finer octaves thin the strand
/// itself instead of merely roughening the edges of a fat one.
///
/// Note this makes the field's characteristic THICKNESS a function of both
/// frequency and sharpness, unlike gfx_fbm_3d where frequency only sets feature
/// size -- which matters when a raymarcher has to resolve it (see gfx/volumetrics.glsl).
///
/// @param p          Sample point (pre-scaled by the caller).
/// @param octaves    Number of fbm octaves.
/// @param lacunarity Frequency multiplier per octave (2.0 is conventional).
/// @param gain       Amplitude multiplier per octave (0.5 is conventional).
/// @param sharpness  Crest exponent; higher is thinner.
float gfx_ridged_fbm_3d(vec3 p, int octaves, float lacunarity, float gain, float sharpness) {
    float sum  = 0.0;
    float amp  = 1.0;
    float norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        float n = gfx_value_noise_3d(p);
        float r = 1.0 - abs(2.0 * n - 1.0);   // crest where n crosses 0.5
        sum  += amp * pow(clamp(r, 0.0, 1.0), sharpness);
        norm += amp;
        p    *= lacunarity;
        amp  *= gain;
    }
    return sum / max(norm, 1e-5);
}

#endif // GFX_NOISE_GLSL
