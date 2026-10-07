#ifndef GFX_AO_COMPOSITE_GLSL
#define GFX_AO_COMPOSITE_GLSL

// Ambient-occlusion compositing helpers shared by the deferred lighting pass, the SSR
// composite (whose indirect-specular subtraction must cancel lighting's term exactly) and
// the forward surface captures. Both functions follow Unity HDRP's AO application
// (core RP AmbientOcclusion.hlsl), so a scene tuned against HDRP's look transfers.

/// Multi-bounce ambient occlusion (Jimenez et al. 2016, sec. 10.2): a cubic fit to
/// path-traced interreflection that tints occluded areas toward the surface albedo
/// instead of flat gray -- bright surfaces bounce light into their own creases.
///
/// @param visibility Scalar AO visibility in [0, 1] (1 = unoccluded).
/// @param albedo     Surface diffuse color the bounce light picks up.
/// @return Per-channel occlusion factor, >= visibility in every channel.
vec3 gfx_gtao_multi_bounce(float visibility, vec3 albedo) {
    vec3 a =  2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c =  2.7552 * albedo + 0.6903;
    return max(vec3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

/// Specular occlusion from ambient occlusion (Lagarde & de Rousiers, "Moving Frostbite to
/// PBR" -- HDRP's GetSpecularOcclusionFromAmbientOcclusion): narrows the occlusion cone
/// for the specular lobe, so smooth surfaces at grazing view keep their reflections while
/// rough, deeply occluded ones lose them.
///
/// @param ndotv     Clamped N.V of the shading point.
/// @param ao        Scalar AO visibility in [0, 1].
/// @param roughness Perceptual roughness in [0, 1].
/// @return Scalar occlusion factor for indirect specular, in [0, 1].
float gfx_specular_occlusion(float ndotv, float ao, float roughness) {
    return clamp(pow(ndotv + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

/// Every occlusion factor a shaded point needs, computed ONE way -- the deferred lighting
/// pass, the SSR composite, the debug channels, the forward path and the editor's viewport
/// shading all call this, so ambient occlusion looks identical wherever it is applied.
struct GfxAoTerms {
    float occlusion;   ///< min(material AO, screen-space AO)
    vec3  diffuse;     ///< multi-bounce factor for indirect diffuse
    vec3  specular;    ///< specular-occlusion cone, multi-bounce tinted by F0, for indirect specular
    vec3  direct;      ///< factor for direct light: diffuse faded in by direct_strength
};

/// @param material_ao      The material's own AO (1 = none).
/// @param ssao             Screen-space AO visibility, 1.0 when SSAO is off / unavailable
///                         (forward surfaces, ssao_enabled false, an editor toggle off).
/// @param direct_strength  How much AO also darkens direct light (ssao_direct_lighting_strength).
GfxAoTerms gfx_ao_terms(float material_ao, float ssao, vec3 albedo, vec3 F0, float ndotv,
                        float roughness, float direct_strength) {
    GfxAoTerms t;
    // Material AO and screen-space AO estimate the same quantity at different scales, so
    // they combine by min() -- multiplying would double-darken where both see a crease.
    t.occlusion = min(material_ao, ssao);
    t.diffuse   = gfx_gtao_multi_bounce(t.occlusion, albedo);
    t.specular  = gfx_gtao_multi_bounce(gfx_specular_occlusion(ndotv, t.occlusion, roughness), F0);
    t.direct    = mix(vec3(1.0), t.diffuse, direct_strength);
    return t;
}

#endif // GFX_AO_COMPOSITE_GLSL
