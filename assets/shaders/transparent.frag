#version 450

// Forward-shaded BLEND transparent pass. Direct lighting reuses
// pixel_lighting.frag's band()/shade_light() formula verbatim -- including
// respecting soft_lighting -- so a transparent object shades identically to
// an opaque one lit by the same light. Indirect lighting now ALSO matches:
// this pass calls the same gfx_indirect_specular() base term pixel_lighting.frag
// uses, then layers on the same screen-space reflection + SSGI bounce delta
// ssr_composite_body.glsl applies to opaque geometry (see gfx/ssr_trace_body.glsl,
// factored out of ssr.frag for exactly this reuse) -- opaque geometry gets that
// delta from a separate fullscreen composite pass over the G-buffer, but BLEND
// geometry never appears in the G-buffer, so this pass folds both steps
// (base ambient + composite delta) into one shader. Differences that remain
// from the opaque path: material comes from a push constant (forward geometry,
// not a G-buffer fetch), there is no SSAO term, no temporal resolve/upsample on
// the traced reflection (see PushConstants' doc), and out_color carries the
// material's alpha instead of being forced to 1.0.

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/ssr_common.glsl>

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

// Set 1: Light UBO -- identical layout to pixel_lighting.frag's
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

// Set 2: Shadow maps -- one directional map, one point cube map (see shadow_map_target.h).
// *Shadow: hardware compareEnable sampler (util::Sampler::shadow()) -- see
// gfx/shadow_sampling.glsl and pixel_lighting.frag's identical binding for why.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCubeShadow point_shadow_map;

// Sets 3/4/5: ssr_pass_'s own trace-input sets (see pixel_render_pipeline.h's
// transparent_extra ExtraSets), the same three sets ssr.frag itself binds at 1/2/3 --
// bound here at 3/4/5 since sets 0-2 above are this pass's own. Only the two bindings
// gfx/ssr_trace_body.glsl actually reads are declared (binding 0 of set 3, g_albedo_ao,
// is unused by the trace and is not declared here -- Vulkan permits a shader to leave
// bindings in its pipeline layout undeclared as long as it never samples them).
layout(set = 3, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 3, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 4, binding = 0) uniform sampler2D u_hiz_map;
layout(set = 5, binding = 0) uniform sampler2D u_scene_color;

#include <gfx/ssr_trace_body.glsl>
#include "indirect_hooks.glsl"

// Push constants: [0, 32) is the per-object material block, byte-identical to
// GBufferPipeline::PushConstants (model/normal_matrix moved to the per-instance vertex
// stream -- see pbr.vert, which this pass's vertex stage uses) and pushed once per draw
// by TransparentPass::push(). [32, 108) is a frame-level lighting/indirect/SSR block
// pushed once per frame by PixelRenderPipeline::record_transparent_() via
// TransparentPass's extra_pc_bytes ctor param -- GLSL permits only one push_constant
// block per stage, so both live in this one struct despite coming from two separate
// push_constants() calls. inv_proj is deliberately NOT pushed (unlike ssr.frag's own
// SsrPushConstants) -- a mat4 here would take this block past 108 bytes, over the
// 128-byte guaranteed-minimum Vulkan push constant budget once padding is counted, so
// it's recomputed once per fragment via inverse(camera.proj) instead.
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;  // unused here -- BLEND materials never alpha-test

    float light_bands;    // discrete N.L shading steps; used when soft_lighting is off
    float spec_threshold; // hard specular highlight cutoff; used when soft_lighting is off
    float soft_lighting;  // != 0 -> smooth Cook-Torrance direct lighting; 0 -> banded/ramped cel look
    float rim_strength;     // 0 disables the rim term -- see pixel_lighting.frag's identical block
    float ambient_intensity; // scales the sky/GI indirect diffuse term
    float sky_intensity;     // scales the sky-gradient indirect specular base
    float ssr_enabled;       // != 0 -> trace gfx_ssr_trace() below; else flat analytic sky only.
                              // Must mirror config_.ssr_enabled exactly: when SSR is off this
                              // frame, hiz_pass_->execute() never runs and u_hiz_map holds stale
                              // data, so the trace itself must be skipped, not merely discounted.
    float ssgi_intensity;    // diffuse colour-bleed strength; 0 = specular-only, matching
                              // ssr_composite_body.glsl's own gate
    float ssgi_distance;     // world-space offset along N for the SSGI bounce lookup

    float ssr_max_distance;
    float ssr_bias_texels;
    float ssr_thickness_min;
    float ssr_thickness_scale;
    float ssr_roughness_cutoff;
    int   ssr_max_iterations;
    int   ssr_max_hiz_mip;
    int   ssr_start_mip;
    int   ssr_min_mip0_steps;
    int   ssr_max_color_mip;
} material;

layout(location = 0) out vec4 out_color;

// Directional shadow: single hard compare -- same kernel as
// pixel_lighting.frag's calc_dir_shadow.
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

// Banded diffuse + hard-thresholded specular, or smooth Cook-Torrance -- identical
// formula and toggle to pixel_lighting.frag's band()/shade_light(); see that file's
// doc for why this engine has exactly one direct-lighting look, now shared verbatim
// by opaque and transparent geometry alike.
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
    float alpha     = clamp(material.albedo.a, 0.0, 1.0);
    float metallic  = material.metallic;
    float roughness = material.roughness;
    float ao        = material.ao;
    const float ssao = 1.0; // no screen-space AO for the forward transparent pass

    vec3 N = normalize(frag_world_normal);
    if (!gl_FrontFacing) N = -N; // correct if cull_mode is ever relaxed to allow back faces

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

        // Only lights.point_lights[0] can be a real shadow caster -- same one-cube-map
        // limitation as pixel_lighting.frag.
        vec3 shadow_bias_pos = frag_world_pos + N * 0.02;
        float shadow = (i == 0u && pl.attenuation.w > 0.5)
            ? calc_point_shadow(pl.position_range.xyz - shadow_bias_pos, range) : 0.0;

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    // Rim light: same accent term pixel_lighting.frag adds, previously missing here.
    if (material.rim_strength > 0.0) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, 3.0) * material.rim_strength;
        Lo += albedo * rim;
    }

    // Indirect lighting, base term: byte-identical to pixel_lighting.frag's own (no floor,
    // no clamp -- that hack is gone; see file doc). No SSGI probes, no reflection probes,
    // so the sky gradient is always both the diffuse irradiance and the specular base.
    vec3 ind_diff = sky_gradient(N) * material.ambient_intensity;
    GfxIndirectSpecular ind = gfx_indirect_specular(frag_world_pos, N, V, F0, roughness, material.sky_intensity);
    vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);
    vec3 ambient = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;

    // Indirect lighting, screen-space delta: what ssr_composite_body.glsl applies to opaque
    // geometry as a SEPARATE fullscreen pass over the G-buffer -- this pass has no G-buffer
    // presence, so both steps happen here in one shader. Traces against the exact same Hi-Z
    // pyramid and prefiltered scene-colour chain ssr.frag itself uses (see the set 3/4/5
    // bindings above), so a BLEND sphere reflects the same floor an opaque one would.
    if (material.ssr_enabled != 0.0) {
        GfxSsrParams sp;
        sp.max_distance    = material.ssr_max_distance;
        sp.bias_texels      = material.ssr_bias_texels;
        sp.thickness_min    = material.ssr_thickness_min;
        sp.thickness_scale  = material.ssr_thickness_scale;
        sp.roughness_cutoff = material.ssr_roughness_cutoff;
        sp.max_iterations   = material.ssr_max_iterations;
        sp.max_hiz_mip      = material.ssr_max_hiz_mip;
        sp.start_mip        = material.ssr_start_mip;
        sp.min_mip0_steps   = material.ssr_min_mip0_steps;
        sp.max_color_mip    = material.ssr_max_color_mip;
        // Jitter is opaque-surface-only: these are near-mirror (roughness ~0.05) surfaces
        // where a lobe sample is the wrong answer, and this forward pass has no temporal
        // resolve to integrate stochastic noise with -- see GfxSsrParams' own doc.
        sp.jitter_strength  = 0.0;
        sp.frame_index      = 0;

        // Same two early-outs ssr.frag applies before ever calling gfx_ssr_trace() --
        // roughness_cutoff (rough surfaces fall back to the flat sky term above) and a
        // degenerate reflection vector (grazing angle / back-facing irregularity).
        vec3 ssr_R = reflect(-V, N);
        GfxSsrHit ssr_hit = (roughness < material.ssr_roughness_cutoff && dot(ssr_R, N) > 0.0)
            ? gfx_ssr_trace(frag_world_pos, N, roughness, inverse(camera.proj), sp)
            : GfxSsrHit(vec3(0.0), 0.0, 0.0, false);
        vec3 ssr_color   = ssr_hit.color;
        float confidence = ssr_hit.confidence;

        vec3 ssr_specular = ssr_color * (ind.F * ind.brdf.x + ind.brdf.y);
        // Clamp: a false-positive SSR hit against nearby dark geometry can leave ssr_specular
        // near zero, making the unclamped delta go negative -- same reasoning as
        // ssr_composite_body.glsl's own clamp.
        ambient = max(ambient + (ssr_specular - confidence * ind.value) * ao * ssao, 0.0);

        // Screen-space diffuse bounce ("SSGI") -- identical formula to
        // ssr_composite_body.glsl's own block; see that file for the reasoning behind each term.
        if (material.ssgi_intensity > 0.0) {
            vec4 bounce_clip = camera.proj * camera.view * vec4(frag_world_pos + N * material.ssgi_distance, 1.0);
            if (bounce_clip.w > 0.0) {
                vec2 bounce_uv = ssr_ndc_to_uv(bounce_clip.xy / bounce_clip.w);
                vec2 edge = smoothstep(vec2(0.0), vec2(0.08), bounce_uv)
                          * smoothstep(vec2(1.0), vec2(0.92), bounce_uv);
                vec3 bounce = textureLod(u_scene_color, clamp(bounce_uv, 0.0, 1.0),
                                         float(material.ssr_max_color_mip)).rgb;
                ambient += kD_ind * albedo * bounce * (edge.x * edge.y) * confidence
                         * material.ssgi_intensity * ao * ssao;
            }
        }
    }

    out_color = vec4(ambient + Lo, alpha);
}
