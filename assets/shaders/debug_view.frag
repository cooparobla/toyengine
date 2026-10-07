#version 450

// debug_view.frag -- replaces the image with one intermediate render buffer, RAW: no
// tonemap, no bloom, no palette/dither/outline, and (see PixelRenderPipeline's
// record_post_chain_) TAA feedback forced to 0 for this frame, so what lands on screen
// is exactly the number the renderer computed, not a temporally-blended or exposure-
// rescaled view of it. See PixelRenderConfig::debug_view / the DebugView enum for the
// full option list.
//
// Shares the exact same descriptor contract as pixel_lighting.frag (sets 0-3: camera,
// light, shadow, G-buffer+SSAO -- here shifted to set 4, after this shader's own extra
// set 3) so the "direct" / "indirect" / "shadows" / "contact_shadows" / "ssao" channels
// are computed from the SAME inputs by the SAME functions the shipped lighting term
// uses (calc_dir_shadow, sky_gradient, gfx_indirect_specular,
// gfx_gtao_multi_bounce) -- a diagnostic built from a different code path could disagree
// with the thing it's meant to be diagnosing.

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/ao_composite.glsl>
#include <gfx/spot_light.glsl>
#include <gfx/ssr_common.glsl>

layout(location = 0) in vec2 in_uv;

// Set 0: Camera UBO -- identical layout to pixel_lighting.frag's.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- see light_ubo_body.glsl, the single copy of this block's layout.
#include "light_ubo_body.glsl"


// Set 2: Shadow maps -- identical to pixel_lighting.frag's.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform sampler2DShadow local_shadow_atlas; // point/spot shadows (gfx/local_shadow.glsl)
layout(set = 2, binding = 3) uniform sampler2D dir_shadow_map_raw;

// Set 3: this shader's own extras -- SsrPass's resolved reflection/SSGI buffers (bound
// to their neutral zero textures when ssr_enabled/ssgi_traced were off at construction,
// same policy as g_ssao below) and the G-buffer depth, for the "depth" channel.
layout(set = 3, binding = 0) uniform sampler2D u_ssr;
layout(set = 3, binding = 1) uniform sampler2D u_ssgi;
layout(set = 3, binding = 2) uniform sampler2D u_depth;
// The resolved contact-shadow occlusion (ContactShadowPass), or its neutral all-zero texture
// when contact shadows were off at construction. Raw, unscaled by strength or per-light
// darkness -- which is what lets the "contact_shadows" channel show the term as it is computed.
layout(set = 3, binding = 3) uniform sampler2D u_contact_shadow;
// G4, the G-buffer's velocity attachment (see gfx/surface/gbuffer_fs.glsl), for "velocity".
layout(set = 3, binding = 4) uniform sampler2D u_velocity;

// Set 4: G-Buffer textures + screen-space AO -- identical layout to pixel_lighting.frag's
// (owned by the FullscreenStage this pass shares gfxcoopa's DeferredLightingPass with).
layout(set = 4, binding = 0) uniform sampler2D g_albedo_ao;
layout(set = 4, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 4, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 4, binding = 3) uniform sampler2D g_ssao;
layout(set = 4, binding = 4) uniform sampler2D g_emissive;

layout(push_constant) uniform DebugViewParams {
    int   channel;              // DebugView enum value -- see DBG_* constants below.
    float light_bands;          // for DBG_DIRECT (band()); matches pixel_lighting.frag's field.
    float spec_threshold;       // for DBG_DIRECT (shade_light()); matches pixel_lighting.frag's field.
    float ambient_intensity;    // for DBG_INDIRECT; matches pixel_lighting.frag's field.
    float sky_intensity;        // for DBG_INDIRECT; matches pixel_lighting.frag's field.
    float soft_lighting;        // for DBG_DIRECT; matches pixel_lighting.frag's field.
    float ssao_direct_strength; // for DBG_DIRECT; matches pixel_lighting.frag's field.
    float camera_near;          // for DBG_DEPTH; same three-field idiom as
    float camera_far;           // pixel_stylize.frag / DofPass's own linearization.
    float camera_is_perspective;
    float editor_ao;            // editor shading (solid / material preview): 1 = apply SSAO, 0 = off
    float editor_xray_alpha;    // editor shading: surface opacity over the backdrop (X-Ray), 1 = opaque
} params;

layout(location = 0) out vec4 out_color;

#include "indirect_hooks.glsl"
#include "pixel_shadow_body.glsl"
#include "editor_shading.glsl"

// DebugView enum values (pixel_render_config.h) this push constant's `channel` carries --
// MUST match that enum member-for-member.
#define DBG_OFF             0
#define DBG_ALBEDO          1
#define DBG_NORMALS         2
#define DBG_ROUGHNESS       3
#define DBG_METALLIC        4
#define DBG_EMISSIVE        5
#define DBG_MATERIAL_AO     6
#define DBG_WORLD_POS       7
#define DBG_DEPTH           8
#define DBG_DIRECT          9
#define DBG_INDIRECT        10
#define DBG_SHADOWS         11
#define DBG_CONTACT_SHADOWS 12
#define DBG_SSAO            13
#define DBG_SSR             14
#define DBG_SSR_CONFIDENCE  15
#define DBG_SSGI            16
// 17-19 (dof, volumetrics, lines) are not drawn by this shader.
#define DBG_SOLID           20
#define DBG_WIREFRAME       21
#define DBG_MATPREVIEW      22
#define DBG_VELOCITY        23

// band()/shade_light() -- byte-for-byte the same as pixel_lighting.frag's own (not a
// shared body: they read `params` fields specific to each shader's own push-constant
// block, so a header would need to abstract that away for no real gain here).
float band(float ndl) {
    if (params.soft_lighting != 0.0) return ndl;
    if (params.light_bands <= 1.0) return ndl;
    return floor(ndl * params.light_bands) / params.light_bands;
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
    if (params.soft_lighting != 0.0) {
        float denom = 4.0 * max(dot(N, V), 0.0) * ndl_raw + 0.0001;
        specular = (NDF * G * F) / denom;
    } else {
        float spec_mask = step(params.spec_threshold, NDF * G);
        specular = F * spec_mask;
    }

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / BRDF_PI + specular) * radiance * ndl * (1.0 - shadow);
}

// Same formula as pixel_stylize.frag's linear_depth(): true view-space distance from
// the camera, from raw Vulkan [0,1] post-projection depth.
float linear_depth(float d) {
    if (params.camera_is_perspective < 0.5) {
        return mix(params.camera_near, params.camera_far, d);
    }
    return params.camera_near * params.camera_far /
           (params.camera_far - d * (params.camera_far - params.camera_near));
}

// Direct-light sum only (dir + point + spot, shadowed) -- no ambient, no emissive.
// Same shade_light()/calc_*_shadow() calls and the same contact-shadow buffer read pixel_lighting.frag's
// main() makes, so DBG_DIRECT can never show a different number than what actually
// reaches the final image's direct term.
vec3 toy_debug_direct(vec3 N, vec3 V, vec3 world_pos, vec3 albedo, float metallic,
                      float roughness, vec3 F0) {
    vec3 Lo = vec3(0.0);

    if (lights.light_counts.x > 0) {
        vec3 L = normalize(-lights.dir_direction.xyz);
        vec3 radiance = lights.dir_color.rgb * lights.dir_direction.w;

        // See pixel_lighting.frag's identical call: the raw point, biased per cascade inside.
        float shadow = calc_dir_shadow(world_pos, N, L);
        if (lights.contact_params.x > 0.0 && shadow < lights.dir_shadow_extra.x) {
            shadow = max(shadow, texture(u_contact_shadow, in_uv).r
                                * lights.contact_params.x * lights.dir_shadow_extra.x);
        }
        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    uint num_points = min(lights.light_counts.y, 16u);
    for (uint i = 0u; i < num_points; ++i) {
        PointLight pl = lights.point_lights[i];
        vec3 frag_to_light = pl.position_range.xyz - world_pos;
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

        // Point shadow from the local-light atlas, when this light holds a slot there
        // (attenuation.w, 1-based) -- see calc_local_shadow().
        float shadow = (pl.attenuation.w > 0.5 && dot(N, L) > 0.0)
            ? calc_local_shadow(pl.attenuation.w, world_pos, N, L) : 0.0;

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    uint num_spots = min(lights.light_counts.z, 8u);
    for (uint i = 0u; i < num_spots; ++i) {
        SpotLight sl = lights.spot_lights[i];
        vec3 frag_to_light = sl.position_range.xyz - world_pos;
        float dist = length(frag_to_light);
        float range = sl.position_range.w;
        if (dist > range || dist < 0.0001) continue;

        vec3 L = frag_to_light / dist;
        float cone = gfx_spot_cone(L, sl.direction_cone.xyz, sl.direction_cone.w, sl.params.y);
        if (cone <= 0.0) continue;

        float sharpness = max(sl.params.x, 0.1);
        float factor = clamp(dist / range, 0.0, 1.0);
        float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        falloff *= falloff;
        float attenuation = falloff / (4.0 * BRDF_PI * (factor * factor + 1.0));
        vec3 radiance = sl.color_intensity.rgb * (sl.color_intensity.w * 0.08) * attenuation * cone;

        // Spot shadow from the local-light atlas (params.z = 1-based slot) -- see calc_local_shadow().
        float shadow = (sl.params.z > 0.5 && dot(N, L) > 0.0)
            ? calc_local_shadow(sl.params.z, world_pos, N, L) : 0.0;

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    return Lo;
}

void main() {
    // Depth doesn't need the G-buffer's material attachments at all -- handle it first
    // so a sky pixel (unwritten G-buffer, depth == 1.0 far-plane clear) still reads as
    // "far", not the "no surface" black every other channel below falls back to.
    if (params.channel == DBG_DEPTH) {
        float d = texture(u_depth, in_uv).r;
        float z = linear_depth(d);
        float range = max(params.camera_far - params.camera_near, 1e-4);
        out_color = vec4(vec3(clamp((z - params.camera_near) / range, 0.0, 1.0)), 1.0);
        return;
    }

    vec4 g0 = texture(g_albedo_ao, in_uv);
    vec4 g1 = texture(g_normal_metallic, in_uv);
    vec3 N = g1.rgb;

    // Editor viewport shading: lighting-independent, so it reads the same in a scene with no
    // lights at all -- the point of authoring in it.
    if (params.channel == DBG_SOLID || params.channel == DBG_WIREFRAME || params.channel == DBG_MATPREVIEW) {
        const bool prev = params.channel == DBG_MATPREVIEW;
        vec3 bg = prev ? editor_matprev_backdrop(in_uv.y) : editor_solid_backdrop(in_uv.y);
        if (dot(N, N) < 0.001) {
            out_color = vec4(params.channel == DBG_WIREFRAME ? bg * 0.6 : bg, 1.0);
            return;
        }
        N = normalize(N);
        if (params.channel == DBG_WIREFRAME) {
            out_color = vec4(bg * 0.6 + vec3(0.035), 1.0);
            return;
        }
        // Screen-space AO exactly as the full render applies it (gfx_ao_terms); the viewer's
        // "Ambient Occlusion" toggle off reads as ssao = 1, which is what the renderer does
        // with ssao_enabled false.
        float ssao = params.editor_ao > 0.5 ? texture(g_ssao, in_uv).r : 1.0;
        vec4 p2 = texture(g_position_roughness, in_uv);
        float ndotv = max(dot(N, normalize(camera.camera_pos - p2.rgb)), 0.0);
        if (!prev) {
            // Solid ignores material AO (it is lighting-independent), not screen-space AO.
            const GfxAoTerms aot = gfx_ao_terms(1.0, ssao, editor_solid_base(g0.rgb), vec3(0.04), ndotv, 0.5,
                                                params.ssao_direct_strength);
            out_color = vec4(mix(bg, editor_solid(N, p2.rgb, camera.camera_pos, g0.rgb, aot), params.editor_xray_alpha), 1.0);
            return;
        }
        const vec3 F0 = mix(vec3(0.04), g0.rgb, clamp(g1.a, 0.0, 1.0));
        const GfxAoTerms aot = gfx_ao_terms(g0.a, ssao, g0.rgb, F0, ndotv, clamp(p2.a, 0.04, 1.0), params.ssao_direct_strength);
        out_color = vec4(mix(bg, editor_material_preview(N, p2.rgb, camera.camera_pos, g0.rgb, g1.a, p2.a, aot,
                                                         texture(g_emissive, in_uv).rgb), params.editor_xray_alpha), 1.0);
        return;
    }

    if (dot(N, N) < 0.001) {
        // Sky / unwritten texel: nothing to show on any material or lighting channel.
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    N = normalize(N);
    vec4 g2 = texture(g_position_roughness, in_uv);

    vec3  albedo    = g0.rgb;
    float ao        = g0.a;
    float metallic  = g1.a;
    vec3  world_pos = g2.rgb;
    float roughness = g2.a;

    vec3 V = normalize(camera.camera_pos - world_pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 result = vec3(0.0);
    switch (params.channel) {
        case DBG_ALBEDO:
            result = albedo;
            break;
        case DBG_NORMALS:
            result = N * 0.5 + 0.5;
            break;
        case DBG_ROUGHNESS:
            result = vec3(roughness);
            break;
        case DBG_METALLIC:
            result = vec3(metallic);
            break;
        case DBG_EMISSIVE:
            result = texture(g_emissive, in_uv).rgb;
            break;
        case DBG_MATERIAL_AO:
            result = vec3(ao);
            break;
        case DBG_WORLD_POS:
            result = fract(world_pos);
            break;
        case DBG_DIRECT: {
            vec3 Lo = toy_debug_direct(N, V, world_pos, albedo, metallic, roughness, F0);
            // Same occlusion scaling pixel_lighting.frag applies to its own Lo -- so this
            // channel shows exactly the direct-light number that reaches the final image,
            // not the unoccluded term.
            float ssao = texture(g_ssao, in_uv).r;
            result = Lo * gfx_ao_terms(ao, ssao, albedo, F0, max(dot(N, V), 0.0), roughness, params.ssao_direct_strength).direct;
            break;
        }
        case DBG_INDIRECT: {
            // Same expression as pixel_lighting.frag's ambient term -- see
            // gfx/ao_composite.glsl's own doc for the HDRP-style occlusion split.
            float ssao = texture(g_ssao, in_uv).r;
            vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
                          * params.ambient_intensity;
            GfxIndirectSpecular ind = gfx_indirect_specular(world_pos, N, V, F0, roughness, params.sky_intensity,
                                                            lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
            vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);
            const GfxAoTerms aot = gfx_ao_terms(ao, ssao, albedo, F0, max(dot(N, V), 0.0), roughness, params.ssao_direct_strength);
            result = kD_ind * albedo * ind_diff * aot.diffuse + ind.value * aot.specular;
            break;
        }
        case DBG_SHADOWS: {
            if (lights.light_counts.x > 0) {
                vec3 L = normalize(-lights.dir_direction.xyz);
                float shadow = calc_dir_shadow(world_pos, N, L);
                result = vec3(shadow);
            }
            break;
        }
        case DBG_CONTACT_SHADOWS: {
            if (lights.light_counts.x > 0) {
                vec3 L = normalize(-lights.dir_direction.xyz);
                result = vec3(texture(u_contact_shadow, in_uv).r);
            }
            break;
        }
        case DBG_SSAO:
            result = vec3(texture(g_ssao, in_uv).r);
            break;
        case DBG_SSR:
            result = texture(u_ssr, in_uv).rgb;
            break;
        case DBG_SSR_CONFIDENCE:
            result = vec3(texture(u_ssr, in_uv).a);
            break;
        case DBG_SSGI:
            result = texture(u_ssgi, in_uv).rgb;
            break;
        case DBG_VELOCITY: {
            // Mid-grey = no motion; 8x gain so a few pixels of motion per frame is visible.
            // Blue marks a surface that was behind the eye last frame (no history, z = -1).
            vec4 v = texture(u_velocity, in_uv);
            result = vec3(0.5 + v.xy * 8.0, v.z < 0.0 ? 0.5 : 0.0);
            break;
        }
        default:
            result = vec3(0.0);
            break;
    }

    out_color = vec4(result, 1.0);
}
