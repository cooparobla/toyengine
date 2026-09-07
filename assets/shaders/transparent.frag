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

// Set 6: forward_globals_'s per-frame lighting/indirect/SSR/refraction UBO (see
// forward_globals.h). Mesh-only -- sdf_forward.frag keeps reading its own SdfGlobals UBO
// instead (see the refraction plan for why SDF glass is excluded). Replaces what used to
// be a per-frame push constant here (TransparentLightingPushConstants) -- see
// TransparentRefractionPushConstants' own doc (pixel_render_pipeline.h) for why: adding
// per-object refraction fields to this file's push-constant block alongside that frame
// block would have gone over the 128-byte guaranteed Vulkan minimum.
layout(set = 6, binding = 0) uniform ForwardGlobalsBlock {
    vec4  lighting0;  // x=light_bands, y=spec_threshold, z=soft_lighting, w=rim_strength
    vec4  lighting1;  // x=ambient_intensity, y=sky_intensity, z=ssr_enabled, w=ssgi_intensity
    vec4  ssr0;       // x=ssgi_distance, y=ssr_max_distance, z=ssr_bias_texels, w=ssr_thickness_min
    vec4  ssr1;       // x=ssr_thickness_scale, y=ssr_roughness_cutoff, zw unused
    ivec4 ssr_steps;  // x=ssr_max_iterations, y=ssr_max_hiz_mip, z=ssr_start_mip, w=ssr_min_mip0_steps
    ivec4 ssr_mip;    // x=ssr_max_color_mip, yzw unused
    vec4  refract0;   // x=enabled, y=strength, z=max_offset, w=chromatic
    vec4  refract1;   // x=blur, y=density, z=fresnel_enabled, w unused
} forward_globals;

#include <gfx/ssr_trace_body.glsl>
#include "indirect_hooks.glsl"
#include "pixel_forward_shading.glsl"
#include "refraction.glsl"

// Push constants: [0, 32) is the per-object material block, byte-identical to
// GBufferPipeline::PushConstants (model/normal_matrix moved to the per-instance vertex
// stream -- see pbr.vert, which this pass's vertex stage uses) and pushed once per draw
// by TransparentPass::push(). [32, 64) is a per-object refraction block (ior/thickness/
// tint/enabled) pushed once per draw by PixelRenderPipeline::record_transparent_() via
// TransparentPass's extra_pc_bytes ctor param -- GLSL permits only one push_constant
// block per stage, so both live in this one struct despite coming from two separate
// push_constants() calls. The frame-level lighting/indirect/SSR block that used to
// occupy this region moved to set 6's UBO above (see TransparentRefractionPushConstants'
// doc for why).
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;  // unused here -- BLEND materials never alpha-test

    vec4  refraction_tint_thickness; // rgb = tint, w = thickness
    vec4  refraction_ior_flags;      // x = ior, y = enabled (!= 0), zw reserved
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
    p.light_bands          = forward_globals.lighting0.x;
    p.spec_threshold       = forward_globals.lighting0.y;
    p.soft_lighting        = forward_globals.lighting0.z;
    p.rim_strength         = forward_globals.lighting0.w;
    p.ambient_intensity    = forward_globals.lighting1.x;
    p.sky_intensity        = forward_globals.lighting1.y;
    p.ssr_enabled          = forward_globals.lighting1.z;
    p.ssgi_intensity       = forward_globals.lighting1.w;
    p.ssgi_distance        = forward_globals.ssr0.x;
    p.ssr_max_distance     = forward_globals.ssr0.y;
    p.ssr_bias_texels      = forward_globals.ssr0.z;
    p.ssr_thickness_min    = forward_globals.ssr0.w;
    p.ssr_thickness_scale  = forward_globals.ssr1.x;
    p.ssr_roughness_cutoff = forward_globals.ssr1.y;
    p.ssr_max_iterations   = forward_globals.ssr_steps.x;
    p.ssr_max_hiz_mip      = forward_globals.ssr_steps.y;
    p.ssr_start_mip        = forward_globals.ssr_steps.z;
    p.ssr_min_mip0_steps   = forward_globals.ssr_steps.w;
    p.ssr_max_color_mip    = forward_globals.ssr_mip.x;

    vec4 shaded = gfx_pixel_forward_shade(frag_world_pos, N, camera.camera_pos, camera.view, camera.proj, mat, p);

    vec3 V = normalize(camera.camera_pos - frag_world_pos);
    vec3 F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

    GfxRefractionMaterial rmat;
    rmat.enabled   = material.refraction_ior_flags.y != 0.0;
    rmat.ior       = material.refraction_ior_flags.x;
    rmat.thickness = material.refraction_tint_thickness.w;
    rmat.tint      = material.refraction_tint_thickness.rgb;

    GfxRefractionParams rp;
    rp.enabled         = forward_globals.refract0.x != 0.0;
    rp.strength        = forward_globals.refract0.y;
    rp.max_offset      = forward_globals.refract0.z;
    rp.chromatic       = forward_globals.refract0.w;
    rp.blur            = forward_globals.refract1.x;
    rp.density         = forward_globals.refract1.y;
    rp.fresnel_enabled = forward_globals.refract1.z != 0.0;

    out_color = gfx_refraction_apply(shaded, frag_world_pos, N, V, mat.roughness, F0,
                                     camera.view, camera.proj, rmat, rp, forward_globals.ssr_mip.x);
}
