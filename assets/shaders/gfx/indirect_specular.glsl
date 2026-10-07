#ifndef GFX_INDIRECT_SPECULAR_GLSL
#define GFX_INDIRECT_SPECULAR_GLSL

// gfx/indirect_specular.glsl -- the indirect-specular term, shared verbatim
// by a consumer's lighting pass and its SSR composite.
//
// The SSR composite's delta is a SUBTRACTION of exactly what the lighting
// pass already added (see gfx/ssr_composite_body.glsl). Historically both
// sites open-coded the same F/brdf/sky_specular/env_specular expression and
// a comment begged the next editor to keep them in sync -- any drift left a
// faint reflection-shaped residue. Calling one function from both sites
// makes agreement structural instead of a matter of convention.
//
// Declares no bindings (same rule as ibl.glsl/ssr_common.glsl): every
// resource the hooks need reaches them through the includer's own
// declarations, so two callers may place their descriptor sets at
// different indices and give their resources different names.

// --- Hooks. Define both BEFORE including this file. ---
vec2 hook_env_brdf(float NdotV, float roughness);
vec3 hook_env_specular(vec3 P, vec3 N, vec3 V, float roughness,
                       vec3 F, vec3 sky_specular);

struct GfxIndirectSpecular {
    vec3 F;     // Fresnel-Schlick-roughness
    vec2 brdf;  // split-sum (scale, bias)
    vec3 sky;   // analytic sky term, pre-hook
    vec3 value; // what the lighting pass adds / the composite subtracts
};

// Explicit-colour overload. See gfx/sky.glsl's own overload for why this
// exists as a second function rather than a signature change: a consumer
// with configurable sky colours passes its own; the 6-arg overload below
// (used by every consumer that doesn't) is unchanged and still free.
GfxIndirectSpecular gfx_indirect_specular(vec3 P, vec3 N, vec3 V, vec3 F0,
                                          float roughness, float sky_intensity,
                                          vec3 sky_zenith, vec3 sky_horizon, vec3 sky_ground)
{
    GfxIndirectSpecular o;
    float NdotV = max(dot(N, V), 0.0);
    o.F    = fresnel_schlick_roughness(NdotV, F0, roughness);
    o.brdf = hook_env_brdf(NdotV, roughness);
    o.sky  = sky_gradient(reflect(-V, N), sky_zenith, sky_horizon, sky_ground)
           * (o.F * o.brdf.x + o.brdf.y) * sky_intensity;
    o.value = hook_env_specular(P, N, V, roughness, o.F, o.sky);
    return o;
}

GfxIndirectSpecular gfx_indirect_specular(vec3 P, vec3 N, vec3 V, vec3 F0,
                                          float roughness, float sky_intensity)
{
    return gfx_indirect_specular(P, N, V, F0, roughness, sky_intensity,
                                 SKY_ZENITH, SKY_HORIZON, SKY_GROUND);
}

#endif // GFX_INDIRECT_SPECULAR_GLSL
