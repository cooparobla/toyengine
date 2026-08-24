// brdf.glsl -- Cook-Torrance BRDF helpers shared by the deferred lighting pass
// (pixel_lighting.frag) and the SSR composite (ssr_composite.frag).
//
// Declares no uniforms, samplers or blocks -- same rule as blendy's ibl.glsl --
// so both includers can place their descriptor sets at whatever index suits
// them. env_brdf_approx() replaces blendy's baked BRDF LUT texture (Karis'
// analytic mobile split-sum fit, SIGGRAPH 2014 "Physically Based Shading on
// Mobile"): close enough to the full integral for this engine's purposes,
// and it lets pixel_lighting.frag and ssr_composite.frag agree on
// F * brdf.x + brdf.y bit-for-bit without shipping a texture asset or wiring
// up a bake step neither pass otherwise needs.

const float BRDF_PI = 3.14159265359;

float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = BRDF_PI * denom * denom;
    return a2 / max(denom, 0.000001);
}

float geometry_schlick_ggx(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.000001);
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return geometry_schlick_ggx(NdotV, roughness) * geometry_schlick_ggx(NdotL, roughness);
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

/// Fresnel-Schlick with a roughness-aware ceiling (Sebastien Lagarde) -- ported
/// verbatim from blendy/assets/shaders/ibl.glsl. Used for all indirect/IBL
/// terms, so rough dielectrics don't blow out to white at grazing angles the
/// way plain Schlick does.
vec3 fresnel_schlick_roughness(float cos_theta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

/// Analytic split-sum environment BRDF (Karis, "Physically Based Shading on
/// Mobile", 2014) -- returns the same (scale, bias) pair a baked BRDF LUT
/// would at texture(brdf_lut, vec2(NdotV, roughness)).rg, without a texture.
/// Callers combine it identically to a real LUT: F * result.x + result.y.
vec2 env_brdf_approx(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}
