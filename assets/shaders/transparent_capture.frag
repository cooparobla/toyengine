#version 450

// Forward-shaded capture of transparent geometry -- NOT a visible draw. Feeds a second
// reflection SOURCE (gfx_ssr_trace_secondary(), gfx/ssr_trace_secondary_body.glsl) so opaque
// reflectors' SSR can find and reflect transparent objects, which otherwise never appear in
// the opaque G-buffer/Hi-Z/scene-colour-mip chain any reflector reads (see
// TransparentCaptureTarget/TransparentCapturePass's own docs for the render-target side).
//
// Direct lighting (band()/shade_light()) and the rim term are LITERAL DUPLICATES of
// transparent.frag's own -- KEEP IN SYNC WITH transparent.frag IF THAT FORMULA EVER CHANGES,
// same tradeoff transparent.frag itself already made against pixel_lighting.frag (see that
// file's doc). Indirect lighting is the BASE term only (sky_gradient() + gfx_indirect_specular(),
// no SSR trace, no IND_SKY_FLOOR-style hack) -- this capture must NOT itself reflect anything
// (avoids the glass-reflects-its-own-reflection recursion a transparent object tracing SSR
// against ITS OWN capture would create), so what an opaque reflector sees when its ray hits a
// transparent object is that object's directly-lit, non-recursive appearance -- consistent
// with how every OTHER hit color this engine's SSR ever samples (gfx_ssr_trace()'s own
// u_scene_color) is the pre-SSR-composite, first-bounce-only scene colour, never a second
// reflection bounce.
//
// No alpha, no blending (see TransparentCapturePass's doc: front-most transparent surface
// wins per pixel by ordinary depth test, this pass owns and writes its own depth).

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- identical layout to transparent.frag's / pixel_lighting.frag's
struct PointLight {
    vec4 position_range;  // xyz = pos, w = range
    vec4 color_intensity; // xyz = color, w = intensity
    vec4 attenuation;     // x=const, y=lin, z=quad, w=cast_shadows (1 or 0)
};

layout(set = 1, binding = 0) uniform LightUBO {
    vec4 dir_direction;
    vec4 dir_color;
    vec4 dir_ambient;
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=unused, z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];
} lights;

// Set 2: Shadow maps -- same bindings as transparent.frag's.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCubeShadow point_shadow_map;

#include "indirect_hooks.glsl"

// Push constants: [0, 32) is the per-object material block, byte-identical to
// TransparentCapturePass::PushConstants (and to TransparentPass::PushConstants -- alpha/
// alpha_cutoff carried along unused, see that struct's own doc). [32, 56) is a frame-level
// lighting/indirect block, pushed once per frame -- deliberately smaller than transparent.frag's
// own [32, 108) block: no ssr_enabled/ssgi/SSR-tuning fields here, since this capture never
// traces its own reflection (see file doc).
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha (unused)
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;  // unused

    float light_bands;
    float spec_threshold;
    float soft_lighting;
    float rim_strength;
    float ambient_intensity;
    float sky_intensity;
} material;

layout(location = 0) out vec4 out_shaded_color;
layout(location = 1) out vec4 out_normal_metallic;
layout(location = 2) out vec4 out_position_roughness;

// Directional shadow: single hard compare -- same kernel as transparent.frag's calc_dir_shadow.
float calc_dir_shadow(vec4 light_space_pos, vec3 N, vec3 L) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    if (proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) {
        return 0.0;
    }

    float bias = max(lights.dir_shadow_params.x * (1.0 - max(dot(N, L), 0.0)), 0.0002);
    return gfx_shadow_dir_hard(dir_shadow_map, proj_coords, bias);
}

float calc_point_shadow(vec3 frag_to_light, float range) {
    vec3 light_to_surface = -frag_to_light;
    vec3 dir = normalize(light_to_surface);
    float current_dist = length(frag_to_light) / range;
    float bias = 0.05 / range;
    return gfx_shadow_cube_hard(point_shadow_map, dir, current_dist, bias);
}

// Identical to transparent.frag's band()/shade_light() -- see file doc.
float band(float ndl) {
    if (material.soft_lighting != 0.0) return ndl;
    if (material.light_bands <= 1.0) return ndl;
    return floor(ndl * material.light_bands) / material.light_bands;
}

vec3 shade_light(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
                 float roughness, vec3 F0, float shadow) {
    vec3 H = normalize(V + L);
    float ndl_raw = max(dot(N, L), 0.0);
    if (ndl_raw <= 0.0) return vec3(0.0);
    float ndl = band(ndl_raw);

    float NDF = distribution_ggx(N, H, roughness);
    float G   = geometry_smith(N, V, L, roughness);
    vec3  F   = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 specular;
    if (material.soft_lighting != 0.0) {
        float denom = 4.0 * max(dot(N, V), 0.0) * ndl_raw + 0.0001;
        specular = (NDF * G * F) / denom;
    } else {
        float spec_mask = step(material.spec_threshold, NDF * G);
        specular = F * spec_mask;
    }

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / BRDF_PI + specular) * radiance * ndl * (1.0 - shadow);
}

void main() {
    vec3 albedo     = material.albedo.rgb;
    float metallic  = material.metallic;
    float roughness = material.roughness;
    float ao        = material.ao;
    const float ssao = 1.0; // no screen-space AO for this forward capture pass either

    vec3 N = normalize(frag_world_normal);
    if (!gl_FrontFacing) N = -N;

    vec3 V = normalize(camera.camera_pos - frag_world_pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    if (lights.light_counts.x > 0) {
        vec3 L = normalize(-lights.dir_direction.xyz);
        vec3 radiance = lights.dir_color.rgb * lights.dir_direction.w;

        float normal_bias_scale = clamp(1.0 - dot(N, L), 0.0, 1.0);
        vec3 biased_pos = frag_world_pos + N * (lights.dir_shadow_params.w * (0.5 + 0.5 * normal_bias_scale));
        vec4 light_space_pos = lights.dir_light_space_matrix * vec4(biased_pos, 1.0);
        float shadow = calc_dir_shadow(light_space_pos, N, L);

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    uint num_points = min(lights.light_counts.y, 16u);
    for (uint i = 0u; i < num_points; ++i) {
        PointLight pl = lights.point_lights[i];
        vec3 frag_to_light = pl.position_range.xyz - frag_world_pos;
        float dist = length(frag_to_light);
        float range = pl.position_range.w;
        if (dist > range || dist < 0.0001) continue;

        vec3 L = frag_to_light / dist;
        float sharpness = max(pl.attenuation.x, 0.1);
        float factor = clamp(dist / range, 0.0, 1.0);
        float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        falloff *= falloff;
        float attenuation = falloff / (4.0 * BRDF_PI * (factor * factor + 1.0));
        vec3 radiance = pl.color_intensity.rgb * (pl.color_intensity.w * 0.08) * attenuation;

        vec3 shadow_bias_pos = frag_world_pos + N * 0.02;
        float shadow = (i == 0u && pl.attenuation.w > 0.5)
            ? calc_point_shadow(pl.position_range.xyz - shadow_bias_pos, range) : 0.0;

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    if (material.rim_strength > 0.0) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, 3.0) * material.rim_strength;
        Lo += albedo * rim;
    }

    // Base indirect term only -- no SSR trace here, see file doc.
    vec3 ind_diff = sky_gradient(N) * material.ambient_intensity;
    GfxIndirectSpecular ind = gfx_indirect_specular(frag_world_pos, N, V, F0, roughness, material.sky_intensity);
    vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);
    vec3 ambient = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;

    out_shaded_color      = vec4(ambient + Lo, 1.0);
    out_normal_metallic   = vec4(N, metallic);
    out_position_roughness = vec4(frag_world_pos, roughness);
}
