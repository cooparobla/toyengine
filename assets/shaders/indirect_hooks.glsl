// indirect_hooks.glsl -- toyengine's hooks for gfx/indirect_specular.glsl:
// no reflection probes, no baked BRDF LUT (this engine has neither). Must
// match ssr_composite.frag's own inline hooks exactly, so the deferred,
// forward and SSR-composite paths all resolve indirect specular the same way.

vec2 hook_env_brdf(float NdotV, float roughness) {
    return env_brdf_approx(NdotV, roughness);
}

vec3 hook_env_specular(vec3 P, vec3 N, vec3 V, float roughness,
                       vec3 F, vec3 sky_specular) {
    return sky_specular;
}
