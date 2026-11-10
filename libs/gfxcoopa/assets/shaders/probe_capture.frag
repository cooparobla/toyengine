#version 450

// Direct-lit forward shading for reflection-probe geometry capture. Pairs
// with pbr.vert, which declares only Set 0 (camera) and reads the model
// matrix from the per-instance vertex stream, so the 32-byte material push
// block below is the pipeline's only push-constant range.
//
// Deliberate limitations:
//   - No shadow sampling. GiSystem::bake() runs once, early, before the main
//     pipeline's shadow maps have ever been rendered (still UNDEFINED at that
//     point), so this shader declares no shadow samplers at all.
//   - No reflection-cubemap read anywhere in this file. Sampling the probe's
//     own cubemap while it's mid-capture would be a read/write hazard against
//     the very image being written, and would recurse. Indirect specular here
//     uses the non-recursive sky_gradient() fallback that lighting shaders
//     use when no probe exists.

#include <gfx/sky.glsl>
#include <gfx/ibl.glsl>   // fresnel_schlick_roughness only
#include <gfx/brdf.glsl>
#include <gfx/spot_light.glsl>

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;   // matches pbr.vert's outputs; rotates u_normal_map into world space

// Set 0: Camera UBO (probe's per-face view/proj/position)
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- a std140 prefix of engine::LightUBO (light_data.h),
// populated directly from the scene by GiSystem (not the main per-frame
// LightData), with dir_shadow_params.z always 0 and no point-light shadow flags.
struct PointLight {
    vec4 position_range;  // xyz = pos, w = range
    vec4 color_intensity; // xyz = color, w = intensity
    vec4 attenuation;     // x=const, y=lin, z=quad, w=cast_shadows (always 0 here)
};

layout(set = 1, binding = 0) uniform LightUBO {
    vec4 dir_direction;
    vec4 dir_color;
    vec4 _reserved_was_dir_ambient; // LightUBO::dir_shadow_extra; unused (no shadows)
    mat4 dir_light_space_matrix; // unused (no shadows)
    vec4 dir_shadow_params;      // z forced 0 by GiSystem

    uvec4 light_counts; // z=num_spot, populated by GiSystem the same way y=num_point is
    PointLight point_lights[16];

    // LightUBO::sky_zenith/horizon/ground, unused here (sky_gradient() is called with its
    // built-in colours) but declared so spot_light_space_matrix lands at its std140 offset.
    vec4 _pad_sky_zenith;
    vec4 _pad_sky_horizon;
    vec4 _pad_sky_ground;

    // Spot Lights -- unshadowed here too, like the rest of this file (no shadow samplers
    // declared at all).
    mat4 spot_light_space_matrix; // unused (no shadows)
    vec4 spot_shadow_params;      // unused (no shadows)
    SpotLight spot_lights[8];
} lights;

// Set 2: BRDF LUT (reused from GiSystem's already-built BRDFLUT)
layout(set = 2, binding = 0) uniform sampler2D u_brdf_lut;

// Set 3: material textures -- see engine::util::MaterialTextureCache. Binding 0 (alpha mask)
// intentionally left undeclared: probe capture, like the main G-buffer's Blend materials,
// performs no alpha test.
layout(set = 3, binding = 1) uniform sampler2D u_albedo_map;
layout(set = 3, binding = 2) uniform sampler2D u_normal_map;
layout(set = 3, binding = 3) uniform sampler2D u_metallic_roughness_map;

// Push constants: identical 32-byte layout to GBufferPipeline/TransparentPass. The model
// matrix comes from the per-instance vertex stream (see pbr.vert).
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
} material;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 albedo_tex = texture(u_albedo_map, frag_uv);
    // glTF packing: metallic in B, roughness in G. Fallback is opaque white, so mr ==
    // vec2(1.0, 1.0) and the two lines below collapse to the untextured values.
    vec2 mr = texture(u_metallic_roughness_map, frag_uv).bg;

    vec3  albedo    = material.albedo.rgb * albedo_tex.rgb;
    float metallic  = material.metallic  * mr.x;
    float roughness = material.roughness * mr.y;
    float ao        = material.ao;

    // Tangent-space normal map rotated into world space. The flat-normal fallback texel
    // (128,128,255) is one step off the exact 127.5 midpoint, so it round-trips to
    // frag_world_normal within ~0.32 degrees rather than bit-identically (see
    // engine::util::MaterialTextureCache).
    vec3 N = normalize(frag_TBN * (texture(u_normal_map, frag_uv).xyz * 2.0 - 1.0));
    vec3 V = normalize(camera.camera_pos - frag_world_pos); // camera_pos == probe position

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    // Direct directional light -- unshadowed.
    if (lights.light_counts.x > 0) {
        vec3 L = normalize(-lights.dir_direction.xyz);
        vec3 H = normalize(V + L);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            float NDF = distribution_ggx(N, H, roughness);
            float G   = geometry_smith(N, V, L, roughness);
            vec3 F    = fresnel_schlick(max(dot(H, V), 0.0), F0);

            vec3 numerator    = NDF * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
            vec3 specular     = numerator / denominator;

            vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

            vec3 radiance = lights.dir_color.rgb * lights.dir_direction.w;
            Lo += (kD * albedo / BRDF_PI + specular) * radiance * NdotL;
        }
    }

    // Direct point lights -- unshadowed.
    uint num_points = min(lights.light_counts.y, 16u);
    for (uint i = 0u; i < num_points; ++i) {
        PointLight pl = lights.point_lights[i];
        vec3 light_pos = pl.position_range.xyz;
        float range = pl.position_range.w;

        vec3 frag_to_light = frag_world_pos - light_pos;
        float dist = length(frag_to_light);
        if (dist > range) continue;

        vec3 L = normalize(-frag_to_light);
        vec3 H = normalize(V + L);

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;

        float dist2 = dist * dist;
        // pl.attenuation.x is a per-light falloff sharpness exponent: ~1 (the component's own
        // default, used when a scene doesn't set this) gives a gradual, realistic fade to the
        // light's range; ~4-8 gives a crisper, more cel-shaded-style cutoff. toyengine's
        // lighting shaders use the same pow(dist / range, sharpness) falloff.
        float sharpness = max(pl.attenuation.x, 0.1);
        float factor = clamp(dist / range, 0.0, 1.0);
        float smooth_falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        smooth_falloff = smooth_falloff * smooth_falloff;
        float attenuation = (1.0 / (4.0 * BRDF_PI * (dist2 + 1.0))) * smooth_falloff;
        vec3 radiance = pl.color_intensity.rgb * (pl.color_intensity.w * 0.08) * attenuation;

        float NDF = distribution_ggx(N, H, roughness);
        float G   = geometry_smith(N, V, L, roughness);
        vec3 F    = fresnel_schlick(max(dot(H, V), 0.0), F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
        vec3 specular     = numerator / denominator;

        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        Lo += (kD * albedo / BRDF_PI + specular) * radiance * NdotL;
    }

    // Direct spot lights -- unshadowed, same dist^2 falloff curve as the point loop above.
    uint num_spots = min(lights.light_counts.z, 8u);
    for (uint i = 0u; i < num_spots; ++i) {
        SpotLight sl = lights.spot_lights[i];
        vec3 light_pos = sl.position_range.xyz;
        float range = sl.position_range.w;

        vec3 frag_to_light = frag_world_pos - light_pos;
        float dist = length(frag_to_light);
        if (dist > range) continue;

        vec3 L = normalize(-frag_to_light);
        float cone = gfx_spot_cone(L, sl.direction_cone.xyz, sl.direction_cone.w, sl.params.y);
        if (cone <= 0.0) continue;

        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;

        float dist2 = dist * dist;
        float sharpness = max(sl.params.x, 0.1);
        float factor = clamp(dist / range, 0.0, 1.0);
        float smooth_falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        smooth_falloff = smooth_falloff * smooth_falloff;
        float attenuation = (1.0 / (4.0 * BRDF_PI * (dist2 + 1.0))) * smooth_falloff;
        vec3 radiance = sl.color_intensity.rgb * (sl.color_intensity.w * 0.08) * attenuation * cone;

        float NDF = distribution_ggx(N, H, roughness);
        float G   = geometry_smith(N, V, L, roughness);
        vec3 F    = fresnel_schlick(max(dot(H, V), 0.0), F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001;
        vec3 specular     = numerator / denominator;

        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        Lo += (kD * albedo / BRDF_PI + specular) * radiance * NdotL;
    }

    // Indirect: the same non-recursive sky fallback the main shaders use when
    // no probe/GI volume exists.
    float NdotV = max(dot(N, V), 0.0);
    vec3  F_ind = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec2  brdf  = texture(u_brdf_lut, vec2(NdotV, roughness)).rg;

    vec3 indirect_diffuse  = sky_gradient(N);
    vec3 indirect_specular = sky_gradient(reflect(-V, N)) * (F_ind * brdf.x + brdf.y);

    vec3 kD_indirect = (vec3(1.0) - F_ind) * (1.0 - metallic);
    vec3 ambient = (kD_indirect * albedo * indirect_diffuse + indirect_specular) * ao;

    out_color = vec4(ambient + Lo, 1.0);
}
