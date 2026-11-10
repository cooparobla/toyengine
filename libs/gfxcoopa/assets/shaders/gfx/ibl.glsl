#ifndef GFX_IBL_GLSL
#define GFX_IBL_GLSL

// gfx/ibl.glsl -- shared image-based-lighting helpers.
//
// This file deliberately declares no uniforms, samplers or blocks besides
// the plain ReflectionProbeData struct. Consumers place the GI descriptor
// set at whatever index suits them and give the resources whatever names
// they want, so every resource is passed in as a function parameter instead.
// That also removes any dependence on include position.

#include <gfx/brdf.glsl>  // fresnel_schlick_roughness

/// Parallax-corrects a reflection vector against the probe's world-space AABB.
vec3 ibl_parallax_correct(vec3 P, vec3 R, vec3 box_min, vec3 box_max, vec3 probe_pos) {
    // Sign-PRESERVING guard against division by zero. A plain
    // `max(R, vec3(0.0001))` would collapse every negative component of R to
    // +1e-4, flipping that slab's near/far distances and producing a wildly
    // wrong t for any ray pointing down a negative axis -- i.e. for most of
    // the hemisphere. sign() is avoided because sign(0.0) == 0.0 would
    // reintroduce the division by zero it is meant to prevent.
    vec3 s = mix(vec3(-1.0), vec3(1.0), step(vec3(0.0), R));
    vec3 safe_R = s * max(abs(R), vec3(1e-4));

    vec3 first_plane  = (box_max - P) / safe_R;
    vec3 second_plane = (box_min - P) / safe_R;
    vec3 furthest     = max(first_plane, second_plane);
    float t = min(min(furthest.x, furthest.y), furthest.z);

    // t <= 0 means P is outside the probe box on the far side; parallax
    // correction is undefined there, so fall back to the raw reflection.
    if (t <= 0.0) return normalize(R);

    return normalize(P + R * t - probe_pos);
}

/// Split-sum indirect specular from a parallax-corrected, prefiltered probe.
/// F is passed in (rather than derived from albedo/metallic) so every call
/// site can agree bit-for-bit on the Fresnel term -- which is what an SSR
/// composite's env_specular subtraction (e.g. toyengine's ssr_composite.frag)
/// depends on.
vec3 ibl_specular_probe(samplerCube refl_map, sampler2D brdf_lut,
                        vec3 P, vec3 N, vec3 V, float roughness, vec3 F,
                        vec3 box_min, vec3 box_max, vec3 probe_pos,
                        float max_mip)
{
    vec3 R = reflect(-V, N);
    vec3 corrected_R = ibl_parallax_correct(P, R, box_min, box_max, probe_pos);

    float mip = clamp(roughness, 0.0, 1.0) * max_mip;
    vec3 prefiltered = textureLod(refl_map, corrected_R, mip).rgb;

    float NdotV = max(dot(N, V), 0.0);
    vec2 brdf = texture(brdf_lut, vec2(NdotV, roughness)).rg;
    return prefiltered * (F * brdf.x + brdf.y);
}

// Must match coopa::gfx::engine::MAX_REFLECTION_PROBES (gfxcoopa/engine/gi/gi_data.h).
#define MAX_REFLECTION_PROBES 4

/// Plain (non-opaque) per-probe data, mirroring ReflectionProbeUniforms
/// (gi_data.h) field-for-field. A type declaration only -- no bindings, same
/// as the rest of this file. Safe to index by a runtime loop variable (unlike
/// the samplerCube parameters below), since it's ordinary UBO data, not an
/// opaque resource array.
struct ReflectionProbeData {
    vec4 probe_position; // xyz = world position
    vec4 box_min;        // xyz = AABB min for parallax correction
    vec4 box_max;        // xyz = AABB max for parallax correction
    vec4 params;         // x = blend_distance, y = importance, z = intensity, w = max_roughness_mip
};

/// Smoothstep falloff weight for how strongly probe `pr` should contribute at
/// surface point P: 0 at/outside the box surface, ramping to 1 over the next
/// blend_distance units moving inward, then scaled by importance as a
/// contribution multiplier (not just a sort key).
float ibl_probe_weight(vec3 P, ReflectionProbeData pr) {
    // Per-axis distance to the nearest face; positive = inside on that axis.
    vec3 dist_to_face = min(pr.box_max.xyz - P, P - pr.box_min.xyz);
    float min_edge = min(min(dist_to_face.x, dist_to_face.y), dist_to_face.z);
    float blend_distance = pr.params.x;
    float importance = pr.params.y;
    return smoothstep(0.0, max(blend_distance, 1e-4), min_edge) * max(importance, 0.0);
}

/// Blends up to MAX_REFLECTION_PROBES probes by box-distance falloff
/// (weighted by importance), then crossfades the combined probe result
/// against sky_specular by total coverage -- so a surface point smoothly
/// transitions from pure sky (no probe nearby), through a blend of whichever
/// probes partially cover it, to a fully-normalized probe blend, with no
/// discontinuity anywhere. This is what makes moving a reflective object
/// across probe boundaries (or out of every probe's influence) fade instead
/// of popping.
///
/// refl_map0..3 are separately-named bindings, not a sampler array: this
/// device does not enable shaderSampledImageArrayDynamicIndexing, so a
/// dynamically-indexed samplerCube[] would be illegal here. Each is sampled
/// with a compile-time-constant index, the same way point-shadow cubemaps
/// are bound as separately-named samplers.
vec3 ibl_specular_probes_blended(
    samplerCube refl_map0, samplerCube refl_map1, samplerCube refl_map2, samplerCube refl_map3,
    sampler2D brdf_lut,
    vec3 P, vec3 N, vec3 V, float roughness, vec3 F,
    ReflectionProbeData probes[MAX_REFLECTION_PROBES], int num_active,
    vec3 sky_specular)
{
    float w0 = (num_active > 0) ? ibl_probe_weight(P, probes[0]) : 0.0;
    float w1 = (num_active > 1) ? ibl_probe_weight(P, probes[1]) : 0.0;
    float w2 = (num_active > 2) ? ibl_probe_weight(P, probes[2]) : 0.0;
    float w3 = (num_active > 3) ? ibl_probe_weight(P, probes[3]) : 0.0;
    float total = w0 + w1 + w2 + w3;

    vec3 weighted_sum = vec3(0.0);
    if (w0 > 0.0) weighted_sum += w0 * ibl_specular_probe(refl_map0, brdf_lut, P, N, V, roughness, F, probes[0].box_min.xyz, probes[0].box_max.xyz, probes[0].probe_position.xyz, probes[0].params.w) * probes[0].params.z;
    if (w1 > 0.0) weighted_sum += w1 * ibl_specular_probe(refl_map1, brdf_lut, P, N, V, roughness, F, probes[1].box_min.xyz, probes[1].box_max.xyz, probes[1].probe_position.xyz, probes[1].params.w) * probes[1].params.z;
    if (w2 > 0.0) weighted_sum += w2 * ibl_specular_probe(refl_map2, brdf_lut, P, N, V, roughness, F, probes[2].box_min.xyz, probes[2].box_max.xyz, probes[2].probe_position.xyz, probes[2].params.w) * probes[2].params.z;
    if (w3 > 0.0) weighted_sum += w3 * ibl_specular_probe(refl_map3, brdf_lut, P, N, V, roughness, F, probes[3].box_min.xyz, probes[3].box_max.xyz, probes[3].probe_position.xyz, probes[3].params.w) * probes[3].params.z;

    vec3 probe_result = (total > 1e-4) ? (weighted_sum / max(total, 1e-4)) : vec3(0.0);
    return mix(sky_specular, probe_result, clamp(total, 0.0, 1.0));
}

#endif // GFX_IBL_GLSL
