#ifndef GFX_SURFACE_CAPTURE_FS_GLSL
#define GFX_SURFACE_CAPTURE_FS_GLSL

// gfx/surface/capture_fs.glsl -- transparent-capture (SSR-secondary-source) fragment
// backbone. See gfx/surface/transparent_fs.glsl's file doc for why this lives in toyengine
// and the GfxTransparentSurface hook contract -- identical here, reusing the SAME struct
// name/shape so one derived shader's gfx_surface_fragment() compiles unchanged against
// both backbones (water.frag's ripple-normal hook applies equally to the visible draw and
// this capture -- see TransparentCapturePass::add_variant()'s doc on why that consistency
// matters for what opaque reflectors see).
//
// Direct lighting here is a duplicate of transparent_fs.glsl's own (see this file's
// original header comment, preserved below) -- a derived shader's hook only touches the
// normal, so that duplication risk is unaffected by this extraction.

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>

// Forward-shaded capture of transparent geometry -- NOT a visible draw. Feeds a second
// reflection SOURCE (gfx_ssr_trace_secondary(), gfx/ssr_trace_secondary_body.glsl) so opaque
// reflectors' SSR can find and reflect transparent objects, which otherwise never appear in
// the opaque G-buffer/Hi-Z/scene-colour-mip chain any reflector reads (see
// TransparentCaptureTarget/TransparentCapturePass's own docs for the render-target side).
//
// Direct lighting (band()/shade_light()) and the rim term are LITERAL DUPLICATES of
// transparent_fs.glsl's own -- KEEP IN SYNC WITH transparent_fs.glsl IF THAT FORMULA EVER
// CHANGES, same tradeoff transparent_fs.glsl itself already made against pixel_lighting.frag
// (see that file's doc). Indirect lighting is the BASE term only (sky_gradient() +
// gfx_indirect_specular(), no SSR trace, no IND_SKY_FLOOR-style hack) -- this capture must
// NOT itself reflect anything (avoids the glass-reflects-its-own-reflection recursion a
// transparent object tracing SSR against ITS OWN capture would create), so what an opaque
// reflector sees when its ray hits a transparent object is that object's directly-lit,
// non-recursive appearance -- consistent with how every OTHER hit color this engine's SSR
// ever samples (gfx_ssr_trace()'s own u_scene_color) is the pre-SSR-composite,
// first-bounce-only scene colour, never a second reflection bounce.
//
// No alpha, no blending (see TransparentCapturePass's doc: front-most transparent surface
// wins per pixel by ordinary depth test, this pass owns and writes its own depth).
//
// No refraction either, deliberately: this pass runs before the lit scene colour exists
// (see PixelRenderPipeline::render()'s ordering), so there is no valid background image
// for a refracted glass object to sample here even if it wanted to -- refraction is applied
// only in the visible forward draw (transparent_fs.glsl/refraction.glsl).

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

// Set 1: Light UBO -- identical layout to transparent_fs.glsl's / pixel_lighting.frag's
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

    // Configurable sky/ambient colour (see IndirectParams in render_features.h). Routed
    // through this UBO rather than PushConstants below (unlike ambient_intensity/
    // sky_intensity) because this shader's push-constant range is already near Vulkan's
    // 128-byte guaranteed minimum (see TransparentCaptureLightingPushConstants' own doc) --
    // 3 vec4s would not fit. Trailing so no field above moves.
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;
} lights;

// Set 2: Shadow maps -- same bindings as transparent_fs.glsl's.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCubeShadow point_shadow_map;

// Set 3: material textures -- see engine::util::MaterialTextureCache and
// transparent_fs.glsl's identical set (there at index 7, since it has sets 3-6 of its own
// that this capture doesn't). Binding 0 (alpha mask) intentionally left undeclared -- this
// capture, like the visible BLEND draw, never alpha-tests.
layout(set = 3, binding = 1) uniform sampler2D u_albedo_map;
layout(set = 3, binding = 2) uniform sampler2D u_normal_map;
layout(set = 3, binding = 3) uniform sampler2D u_metallic_roughness_map;

#include "indirect_hooks.glsl"

// Push constants: [0, 32) is the per-object material block, byte-identical to
// TransparentCapturePass::PushConstants (and to TransparentPass::PushConstants -- alpha/
// alpha_cutoff carried along unused, see that struct's own doc). [32, 64) is the standard
// trailing "surface" block every surface-shader backbone appends (gfx_time/gfx_params --
// see GBufferPipeline::PushConstants' doc); this capture shares the SAME vertex entry
// point as transparent_fs.glsl for a given derived shader (see TransparentCapturePass's
// add_variant() doc), so it needs the same gfx_params reach for its own hook. [64, 88) is
// a frame-level lighting/indirect block, pushed once per frame -- deliberately smaller
// than transparent_fs.glsl's own [64, 140) block: no ssr_enabled/ssgi/SSR-tuning fields
// here, since this capture never traces its own reflection (see file doc).
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha (unused)
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;  // unused

    vec4  gfx_time;   // x=time, y=delta_time, z=frame_index, w=spare
    vec4  gfx_params; // four author-defined floats; see the surface shader's own doc

    float light_bands;
    float spec_threshold;
    float soft_lighting;
    float rim_strength;
    float ambient_intensity;
    float sky_intensity;
} material;

// See gfx/surface/gbuffer_vs.glsl's identical aliases.
vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

layout(location = 0) out vec4 out_shaded_color;
layout(location = 1) out vec4 out_normal_metallic;
layout(location = 2) out vec4 out_position_roughness;

/// Same hook contract as gfx/surface/transparent_fs.glsl's GfxTransparentSurface -- see
/// that file's doc for why it's just the normal (plus position/uv for context), not the
/// full GfxSurface set.
struct GfxTransparentSurface {
    vec3 normal_ws;
    vec3 position_ws;
    vec2 uv;
};

#ifdef GFX_SURFACE_FRAGMENT
void gfx_surface_fragment(inout GfxTransparentSurface s);
#else
void gfx_surface_fragment(inout GfxTransparentSurface s) {}
#endif

// calc_dir_shadow()/calc_point_shadow() now come from pixel_shadow_body.glsl -- see that
// file's doc; this used to be a hand-rolled hard-compare-only duplicate.
#include "pixel_shadow_body.glsl"

// Identical to transparent_fs.glsl's band()/shade_light() -- see file doc.
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
    vec4 albedo_tex = texture(u_albedo_map, frag_uv);
    // glTF packing: metallic in B, roughness in G. Fallback is opaque white, so mr ==
    // vec2(1.0, 1.0) and the two lines below collapse to today's untextured values.
    vec2 mr = texture(u_metallic_roughness_map, frag_uv).bg;

    vec3 albedo     = material.albedo.rgb * albedo_tex.rgb;
    float metallic  = material.metallic  * mr.x;
    float roughness = material.roughness * mr.y;
    float ao        = material.ao;
    const float ssao = 1.0; // no screen-space AO for this forward capture pass either

    // Tangent-space normal map rotated into world space -- see gbuffer_fs.glsl's identical
    // derivation and its doc on why the flat-normal fallback round-trips to frag_world_normal
    // (up to ~0.32 degrees, not bit-identical).
    vec3 N = normalize(frag_TBN * (texture(u_normal_map, frag_uv).xyz * 2.0 - 1.0));
    if (!gl_FrontFacing) N = -N;

    GfxTransparentSurface s;
    s.normal_ws   = N;
    s.position_ws = frag_world_pos;
    s.uv          = frag_uv;
    gfx_surface_fragment(s);
    N = normalize(s.normal_ws);

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
    vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
                  * material.ambient_intensity;
    GfxIndirectSpecular ind = gfx_indirect_specular(frag_world_pos, N, V, F0, roughness, material.sky_intensity,
                                                    lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
    vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);
    vec3 ambient = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;

    out_shaded_color      = vec4(ambient + Lo, 1.0);
    out_normal_metallic   = vec4(N, metallic);
    out_position_roughness = vec4(frag_world_pos, roughness);
}

#endif // GFX_SURFACE_CAPTURE_FS_GLSL
