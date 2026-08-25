// indirect_hooks.glsl -- toyengine's hooks for gfx/indirect_specular.glsl:
// no reflection probes, no baked BRDF LUT (this engine has neither) -- must
// match gfxcoopa's base ssr_composite.frag's own (inline) hooks exactly,
// since toyengine ships no override of that shader and falls back to the
// base copy at runtime (see assets/shaders/.glslc_flags / ShaderLibrary).

vec2 hook_env_brdf(float NdotV, float roughness) {
    return env_brdf_approx(NdotV, roughness);
}

vec3 hook_env_specular(vec3 P, vec3 N, vec3 V, float roughness,
                       vec3 F, vec3 sky_specular) {
    return sky_specular;
}
