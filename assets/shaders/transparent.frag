#version 450

// Forward-shaded BLEND transparent pass. The actual shading formula (direct
// lighting, rim, base indirect term, SSR trace + SSGI bounce) now lives in
// pixel_forward_shading.glsl's gfx_pixel_forward_shade() -- extracted so
// sdf_forward.frag (the BLEND SdfRenderer forward pass) can share it
// byte-for-byte, so a glass mesh and a glass SDF blob shade identically
// instead of risking the kind of formula drift transparent_capture.frag's
// own duplicated copy already has to warn about keeping in sync by hand.
// This file now owns only: its descriptor/push-constant declarations (the
// ABI a caller/loader depends on, unchanged from before this extraction) and
// translating them into the shared function's GfxForwardMaterial/
// GfxForwardLightingParams structs.

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
#include "pixel_forward_shading.glsl"

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

void main() {
    vec3 N = normalize(frag_world_normal);
    if (!gl_FrontFacing) N = -N; // correct if cull_mode is ever relaxed to allow back faces

    GfxForwardMaterial mat;
    mat.albedo    = material.albedo.rgb;
    mat.alpha     = material.albedo.a;
    mat.metallic  = material.metallic;
    mat.roughness = material.roughness;
    mat.ao        = material.ao;

    GfxForwardLightingParams p;
    p.light_bands          = material.light_bands;
    p.spec_threshold       = material.spec_threshold;
    p.soft_lighting        = material.soft_lighting;
    p.rim_strength         = material.rim_strength;
    p.ambient_intensity    = material.ambient_intensity;
    p.sky_intensity        = material.sky_intensity;
    p.ssr_enabled          = material.ssr_enabled;
    p.ssgi_intensity       = material.ssgi_intensity;
    p.ssgi_distance        = material.ssgi_distance;
    p.ssr_max_distance     = material.ssr_max_distance;
    p.ssr_bias_texels      = material.ssr_bias_texels;
    p.ssr_thickness_min    = material.ssr_thickness_min;
    p.ssr_thickness_scale  = material.ssr_thickness_scale;
    p.ssr_roughness_cutoff = material.ssr_roughness_cutoff;
    p.ssr_max_iterations   = material.ssr_max_iterations;
    p.ssr_max_hiz_mip      = material.ssr_max_hiz_mip;
    p.ssr_start_mip        = material.ssr_start_mip;
    p.ssr_min_mip0_steps   = material.ssr_min_mip0_steps;
    p.ssr_max_color_mip    = material.ssr_max_color_mip;

    out_color = gfx_pixel_forward_shade(frag_world_pos, N, camera.camera_pos, camera.view, camera.proj, mat, p);
}
