#ifndef GFX_TOON_GLSL
#define GFX_TOON_GLSL

// gfx/toon.glsl -- per-material toon (cel) shading, layered ON TOP of the Cook-Torrance
// BRDF rather than replacing it.
//
// A material opts in with PBRMaterial::toon_bands and friends; those settings reach the
// deferred lighting pass as one packed integer in the G-buffer's emissive alpha (see
// PBRMaterial::gpu_toon_code() for the bit layout, which gfx_toon_unpack() below is the
// decoder for), and the scene-wide continuous knobs come from LightUBO's
// toon_params0/toon_params1. So a toon prop and a PBR prop can stand side by side in one
// frame, lit by the same lights.
//
// What makes this "toon" rather than "a different shader": only the *edges* of the direct
// terms change.
//   * gfx_toon_ramp() quantizes a light's VISIBILITY -- N.L already folded with the shadow
//     it receives -- so the bands follow the cascaded shadow map's PCSS penumbra, the
//     screen-space contact-shadow march and the cloud-shadow map. A toon surface walking
//     into a soft shadow gets its bands bent by that shadow's real shape.
//   * gfx_toon_specular() steps the GGX lobe the PBR path already evaluated, at a threshold
//     normalized by that lobe's own peak -- so the highlight keeps its physically-correct
//     position, size and F0 colour, and only its edge goes hard.
//   * gfx_toon_rim() is per-light, so the accent sits on the side the light is on, takes
//     that light's colour, and vanishes in shadow.
// Everything a surface gets from its environment INDIRECTLY -- the sky-gradient ambient, the
// AO composite, screen-space reflections and the SSGI bounce -- is deliberately left
// continuous, which is what keeps a banded object sitting in its scene instead of looking
// pasted onto it, and what gives its dark bands the sky's own colour for free.
//
// Pure functions: declares no uniforms, samplers or blocks (the gfx/brdf.glsl convention),
// so every includer places its descriptor sets wherever it likes and passes the two LightUBO
// vec4s in. Requires <gfx/brdf.glsl> for BRDF_PI.

/// The four band-edge softness values a material's packed 2-bit softness index selects.
/// Mirrors PBRMaterial::kToonSoftness, which is what does the snapping on the CPU -- the two
/// tables must stay in step or a material renders with a softness it never asked for.
const float GFX_TOON_SOFTNESS[4] = float[4](0.0, 0.15, 0.35, 0.70);

#define GFX_TOON_SPEC_PBR     0
#define GFX_TOON_SPEC_HARD    1
#define GFX_TOON_SPEC_TWOSTEP 2
#define GFX_TOON_SPEC_OFF     3

/// One surface's decoded toon settings, plus the frame's scene-wide tuning. Built once per
/// fragment by gfx_toon_unpack() and passed down to the shading calls below.
struct GfxToon {
    // --- per material (the packed code) ---
    float bands;      ///< Diffuse bands; 0 means this material is not banded at all.
    int   spec;       ///< One of the GFX_TOON_SPEC_* modes above.
    float rim;        ///< Per-light rim strength; 0 disables it.
    float softness;   ///< Band-edge width, as a share of one band.
    bool  outline;    ///< The stylize pass's per-material outline flag (unread here).
    // --- scene-wide (LightUBO toon_params0/1) ---
    float spec_threshold;
    float spec_edge;
    float rim_power;
    float strength;   ///< Master cross-fade toward plain PBR; 0 disables toon frame-wide.
    vec3  shadow_tint;
    bool  band_attenuation;
};

/**
 * Decodes the G-buffer's emissive alpha into the settings above.
 *
 * The code is stored in a half-float channel, which represents integers up to 2048 exactly,
 * so the +0.5 round-trips the authored value rather than risking a floor() off-by-one on a
 * value that arrived as 1023.99997.
 */
GfxToon gfx_toon_unpack(float code, vec4 p0, vec4 p1) {
    uint c = uint(max(code, 0.0) + 0.5);
    uint bands_sel = c & 7u;

    GfxToon t;
    t.bands    = bands_sel == 0u ? 0.0 : float(bands_sel + 1u);   // 1..7 -> 2..8 bands
    t.spec     = int((c >> 3) & 3u);
    t.rim      = float((c >> 5) & 7u) * 0.25;
    t.softness = GFX_TOON_SOFTNESS[int((c >> 8) & 3u)];
    t.outline  = ((c >> 10) & 1u) != 0u;

    t.spec_threshold    = p0.x;
    t.spec_edge         = p0.y;
    t.rim_power         = p0.z;
    t.strength          = clamp(p0.w, 0.0, 1.0);
    t.shadow_tint       = p1.rgb;
    t.band_attenuation  = p1.w > 0.5;
    return t;
}

/// True when this fragment should take the toon path. False for every plain PBR material,
/// and for every material at once when the scene's master strength is 0 -- which is what
/// lets an A/B capture or a render test switch the whole look off without editing materials.
bool gfx_toon_active(GfxToon t) { return t.bands > 1.5 && t.strength > 0.0; }

/**
 * Quantizes [0,1] into `t.bands` discrete steps.
 *
 * A smoothed round() rather than a floor(): the plateaus are {0, 1/b, ..., 1}, so the dark
 * side still reaches black and the lit side still reaches full brightness, and the mean over
 * the ramp tracks the smooth input. A floor() quantizer instead maps every value in a band
 * down to that band's floor, which costs half a band of brightness everywhere and is why a
 * naively cel-shaded frame reads as uniformly murkier than its PBR reference.
 *
 * The step's edge is half the authored softness, or one screen pixel of the ramp's own
 * gradient where that is wider -- so the terminator antialiases instead of stair-stepping
 * along the band boundary, at no authoring cost. Taking the max rather than the sum keeps a
 * deliberately hard terminator hard wherever the geometry is steep enough to afford it.
 *
 * The fwidth() runs under non-uniform control flow (the caller reaches this only for a toon
 * material, and a quad straddling the boundary with a PBR neighbour mixes the two), where GLSL
 * leaves the derivative's value undefined. Hence the clamp, which is a correctness bound and
 * not just tidiness: on such a quad the width may be any value at all, and the clamp confines
 * the consequence to one seam texel's edge being softer or harder than its neighbour's -- never
 * a NaN, an inverted smoothstep, or a band that swallows the whole ramp.
 */
float gfx_toon_ramp(float x, GfxToon t) {
    float v = clamp(x, 0.0, 1.0);
    float s = v * t.bands;
    float i = floor(s);
    float w = clamp(max(t.softness, fwidth(s)) * 0.5, 1.0e-4, 0.5);
    float q = (i + smoothstep(0.5 - w, 0.5 + w, s - i)) / t.bands;
    return mix(v, clamp(q, 0.0, 1.0), t.strength);
}

/**
 * The hard-edged toon highlight, cut out of the lobe the PBR path already evaluated.
 *
 * `ndf` is distribution_ggx()'s raw value, normalized here by that lobe's own peak
 * (D at N.H == 1 is `1 / (pi * a^2)` for `a = roughness^2`). That normalization is the whole
 * point: the raw lobe value peaks near 1e5 on a near-mirror and near 1 on a matte surface, so
 * a threshold compared against it directly means something completely different at each
 * roughness. Against the normalized value, one threshold puts the step at the same *share* of
 * the lobe everywhere, and the highlight's size still grows with roughness exactly as PBR's
 * does.
 *
 * @param pbr_specular The caller's own Cook-Torrance term, returned unchanged for
 *        GFX_TOON_SPEC_PBR and cross-faded toward for a master strength below 1.
 */
vec3 gfx_toon_specular(GfxToon t, float ndf, vec3 F, float roughness, vec3 pbr_specular) {
    if (t.spec == GFX_TOON_SPEC_PBR) return pbr_specular;
    if (t.spec == GFX_TOON_SPEC_OFF) return mix(pbr_specular, vec3(0.0), t.strength);

    float a    = roughness * roughness;
    float peak = 1.0 / max(BRDF_PI * a * a, 1.0e-9);
    float d    = clamp(ndf / peak, 0.0, 1.0);
    float e    = max(t.spec_edge, 1.0e-4);
    float mask = smoothstep(t.spec_threshold, t.spec_threshold + e, d);

    // Two-step: a dimmer shelf outside the bright core, at half the threshold -- the
    // core-plus-halo highlight hand-painted cel animation uses on glossy surfaces.
    if (t.spec == GFX_TOON_SPEC_TWOSTEP) {
        float outer = smoothstep(t.spec_threshold * 0.5, t.spec_threshold * 0.5 + e, d);
        mask = max(mask, outer * 0.35);
    }
    return mix(pbr_specular, F * mask, t.strength);
}

/**
 * Per-light rim accent, as a scalar the caller multiplies by that light's radiance and banded
 * visibility. Returning it per light rather than adding one flat term after the light loop is
 * what makes the accent land on the side the light is on, take the light's colour, and
 * disappear where the light is shadowed.
 */
float gfx_toon_rim(GfxToon t, float ndv) {
    if (t.rim <= 0.0) return 0.0;
    return pow(1.0 - clamp(ndv, 0.0, 1.0), max(t.rim_power, 0.01)) * t.rim * t.strength;
}

/**
 * Albedo multiplier for the banded diffuse: the dark bands are pulled toward the scene's
 * shadow tint, the fully-lit band is left alone. Returns 1 for the default white tint.
 */
vec3 gfx_toon_shadow_tint(GfxToon t, float vis) {
    return mix(mix(vec3(1.0), t.shadow_tint, t.strength), vec3(1.0), clamp(vis, 0.0, 1.0));
}

#endif // GFX_TOON_GLSL
