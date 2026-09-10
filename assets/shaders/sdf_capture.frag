#version 450

// Raymarches one BLEND SdfRenderer's directly-lit appearance into
// TransparentCaptureTarget -- the SDF analogue of transparent_capture.frag,
// NOT a visible draw of its own (see SdfCapturePass's doc). Direct lighting
// (band()/shade_light()) and the rim term are LITERAL DUPLICATES of
// transparent.frag's/transparent_capture.frag's own -- KEEP IN SYNC IF THAT
// FORMULA EVER CHANGES. This capture deliberately does NOT call
// pixel_forward_shading.glsl's gfx_pixel_forward_shade(): that function
// always statically references the SSR trace machinery (gfx_ssr_trace() and
// friends), which would force this pass to also bind Hi-Z/scene-colour
// sampler sets it has no use for -- this capture must never itself trace a
// reflection anyway (avoids the glass-reflects-its-own-reflection recursion
// a transparent object tracing SSR against ITS OWN capture would create), so
// duplicating the smaller base-indirect-only formula, exactly as
// transparent_capture.frag already does relative to transparent.frag, stays
// the leaner choice here too.

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/sdf.glsl>

layout(location = 0) in flat uint frag_renderer_index;
layout(location = 1) in vec2 frag_ndc; // see sdf_quad.vert's doc

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- identical layout to transparent_capture.frag's
struct PointLight {
    vec4 position_range;  // xyz = pos, w = range
    vec4 color_intensity; // xyz = color, w = intensity
    vec4 attenuation;     // x=const, y=lin, z=quad, w=cast_shadows (1 or 0)
};

layout(set = 1, binding = 0) uniform LightUBO {
    vec4 dir_direction;
    vec4 dir_color;
    vec4 dir_shadow_extra; // x=shadow_intensity, y=point_pcf_radius, z=pcf_samples, w=frame_offset -- see LightUBO's C++ doc (light_data.h)
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=pcf_radius_texels (0=hard), z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];

    // Configurable sky/ambient colour (see IndirectParams in render_features.h).
    // Trailing so no field above moves -- std140 only requires a matching prefix.
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;
} lights;

// Set 2: Shadow maps
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCubeShadow point_shadow_map;

// Set 3: SdfData -- only lighting0/lighting1's first two components are read here (this
// capture never traces SSR, so ssr0/ssr1/ssr_steps/ssr_mip go unused -- declared anyway to
// match SdfData's one shared layout across all four consumers, same reasoning
// transparent_capture.frag's own PushConstants gives for carrying unused alpha/alpha_cutoff).
layout(set = 3, binding = 0) uniform SdfGlobalsBlock {
    mat4  inv_view_proj;
    vec4  camera_pos;
    vec4  lighting0;   // x=light_bands, y=spec_threshold, z=soft_lighting, w=rim_strength
    vec4  lighting1;   // x=ambient_intensity, y=sky_intensity, zw unused here
    vec4  ssr0;
    vec4  ssr1;
    ivec4 ssr_steps;
    ivec4 ssr_mip;
} sdf_globals;
layout(std430, set = 3, binding = 1) readonly buffer SdfRendererBuffer { SdfRendererGpu sdf_renderers[]; };
layout(std430, set = 3, binding = 2) readonly buffer SdfShapeBuffer   { SdfShapeGpu sdf_shapes[]; };

#include "indirect_hooks.glsl"
#include <gfx/sdf_scene_body.glsl>

layout(location = 0) out vec4 out_shaded_color;
layout(location = 1) out vec4 out_normal_metallic;
layout(location = 2) out vec4 out_position_roughness;

// calc_dir_shadow()/calc_point_shadow() now come from pixel_shadow_body.glsl -- see that
// file's doc; this used to be a hand-rolled hard-compare-only duplicate.
#include "pixel_shadow_body.glsl"

float band(float ndl, float soft_lighting, float light_bands) {
    if (soft_lighting != 0.0) return ndl;
    if (light_bands <= 1.0) return ndl;
    return floor(ndl * light_bands) / light_bands;
}

vec3 shade_light(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
                 float roughness, vec3 F0, float shadow, float soft_lighting,
                 float light_bands, float spec_threshold) {
    vec3 H = normalize(V + L);
    float ndl_raw = max(dot(N, L), 0.0);
    if (ndl_raw <= 0.0) return vec3(0.0);
    float ndl = band(ndl_raw, soft_lighting, light_bands);

    float NDF = distribution_ggx(N, H, roughness);
    float G   = geometry_smith(N, V, L, roughness);
    vec3  F   = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 specular;
    if (soft_lighting != 0.0) {
        float denom = 4.0 * max(dot(N, V), 0.0) * ndl_raw + 0.0001;
        specular = (NDF * G * F) / denom;
    } else {
        float spec_mask = step(spec_threshold, NDF * G);
        specular = F * spec_mask;
    }

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / BRDF_PI + specular) * radiance * ndl * (1.0 - shadow);
}

void main() {
    SdfRendererGpu r = sdf_renderers[frag_renderer_index];

    vec3 ro, rd;
    gfx_sdf_ray_from_clip(sdf_globals.inv_view_proj, frag_ndc, ro, rd);

    vec2 tbounds = gfx_sdf_aabb_intersect(ro, rd, r.bounds_min.xyz, r.bounds_max.xyz);
    if (tbounds.x > tbounds.y || tbounds.y < 0.0) discard;
    float t_start = max(tbounds.x, 0.0);

    GfxSdfHit hit = gfx_sdf_march_renderer(frag_renderer_index, ro, rd, t_start, tbounds.y,
                                           int(r.range.z), r.march.x);
    if (!hit.hit) discard;

    vec3 N = gfx_sdf_normal_renderer(frag_renderer_index, hit.pos, r.march.y);
    if (dot(N, rd) > 0.0) N = -N;

    // Standard opaque-style depth test/write among transparent objects only -- see
    // TransparentCaptureTarget's own doc for why this is never cross-tested against the
    // opaque G-buffer.
    vec4 clip = camera.proj * camera.view * vec4(hit.pos, 1.0);
    gl_FragDepth = clip.z / clip.w;

    vec3  albedo    = r.albedo_alpha.rgb;
    float metallic  = r.mr_ao_cutoff.x;
    float roughness = r.mr_ao_cutoff.y;
    float ao        = r.mr_ao_cutoff.z;

    float light_bands       = sdf_globals.lighting0.x;
    float spec_threshold    = sdf_globals.lighting0.y;
    float soft_lighting     = sdf_globals.lighting0.z;
    float rim_strength      = sdf_globals.lighting0.w;
    float ambient_intensity = sdf_globals.lighting1.x;
    float sky_intensity     = sdf_globals.lighting1.y;

    vec3 V = normalize(camera.camera_pos - hit.pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    if (lights.light_counts.x > 0) {
        vec3 L = normalize(-lights.dir_direction.xyz);
        vec3 radiance = lights.dir_color.rgb * lights.dir_direction.w;

        float normal_bias_scale = clamp(1.0 - dot(N, L), 0.0, 1.0);
        vec3 biased_pos = hit.pos + N * (lights.dir_shadow_params.w * (0.5 + 0.5 * normal_bias_scale));
        vec4 light_space_pos = lights.dir_light_space_matrix * vec4(biased_pos, 1.0);
        float shadow = calc_dir_shadow(light_space_pos, N, L);

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow,
                          soft_lighting, light_bands, spec_threshold);
    }

    uint num_points = min(lights.light_counts.y, 16u);
    for (uint i = 0u; i < num_points; ++i) {
        PointLight pl = lights.point_lights[i];
        vec3 frag_to_light = pl.position_range.xyz - hit.pos;
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

        vec3 shadow_bias_pos = hit.pos + N * 0.02;
        float shadow = (i == 0u && pl.attenuation.w > 0.5)
            ? calc_point_shadow(pl.position_range.xyz - shadow_bias_pos, range) : 0.0;

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow,
                          soft_lighting, light_bands, spec_threshold);
    }

    if (rim_strength > 0.0) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, 3.0) * rim_strength;
        Lo += albedo * rim;
    }

    vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
                  * ambient_intensity;
    GfxIndirectSpecular ind = gfx_indirect_specular(hit.pos, N, V, F0, roughness, sky_intensity,
                                                    lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
    vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);
    vec3 ambient = (kD_ind * albedo * ind_diff + ind.value) * ao;

    out_shaded_color        = vec4(ambient + Lo, 1.0);
    out_normal_metallic     = vec4(N, metallic);
    out_position_roughness  = vec4(hit.pos, roughness);
}
