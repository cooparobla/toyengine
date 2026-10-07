#version 450

// SSR composite: no reflection probes, no GI. env_specular degrades to the
// analytic sky term, computed with env_brdf_approx() (no baked BRDF LUT
// texture needed) rather than a real probe blend. The hooks below must stay
// identical to indirect_hooks.glsl, which the deferred/forward shading paths
// include instead.

#include <gfx/sky.glsl>
#include <gfx/ssr_common.glsl>
#include <gfx/brdf.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/ssr_composite_pc.glsl>

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer + screen-space AO
layout(set = 1, binding = 0) uniform sampler2D g_albedo_ao;
layout(set = 1, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 1, binding = 3) uniform sampler2D g_ssao;

// Set 2: Resolved SSR map (temporal-resolved raymarch output)
layout(set = 2, binding = 0) uniform sampler2D u_ssr_map;

// Set 3: Deferred-lit scene colour (raw, for compositing into), its
// prefiltered mip chain (blurry, for the fallback SSGI diffuse-bounce tap),
// and the temporally-resolved traced-SSGI buffer (a permanent 1x1 zero
// fallback when the trace stage is off -- see SsrPass's ssgi_frag_spv).
layout(set = 3, binding = 0) uniform sampler2D u_scene_color;
layout(set = 3, binding = 1) uniform sampler2D u_scene_color_mips;
layout(set = 3, binding = 2) uniform sampler2D u_ssgi_map;

// --- Hooks: no probes, analytic BRDF (see gfx/indirect_specular.glsl) ---
vec2 hook_env_brdf(float NdotV, float roughness) {
    return env_brdf_approx(NdotV, roughness);
}
vec3 hook_env_specular(vec3 P, vec3 N, vec3 V, float roughness,
                       vec3 F, vec3 sky_specular) {
    return sky_specular;
}

#include <gfx/ssr_composite_body.glsl>
