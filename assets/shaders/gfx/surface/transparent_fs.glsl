#ifndef GFX_SURFACE_TRANSPARENT_FS_GLSL
#define GFX_SURFACE_TRANSPARENT_FS_GLSL

// gfx/surface/transparent_fs.glsl -- forward BLEND transparent fragment backbone.
//
// Lives in toyengine (like transparent_vs.glsl): this is toyengine's own forward-shading
// pipeline (SSR trace inputs, forward_globals_ UBO, refraction), not something gfxcoopa's
// base library owns. See gbuffer_fs.glsl for the include-order contract this follows;
// GFX_SURFACE_FRAGMENT gates gfx_surface_fragment() the same way GFX_SURFACE_VERTEX gates
// gfx_surface_vertex() in the *_vs.glsl backbones. The hook may perturb the world-space
// normal BEFORE lighting/SSR/refraction all consume it -- e.g. water's animated ripple
// normals -- but does not touch albedo/roughness/etc.; those stay backbone-owned so every
// derived transparent shader keeps the same BRDF and refraction behaviour.

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
    vec4 dir_shadow_extra; // x=shadow_intensity, y=point_pcf_radius, z=pcf_samples, w=frame_offset -- see LightUBO's C++ doc (light_data.h)
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=pcf_radius_texels (0=hard), z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];

    // Configurable sky/ambient colour (see IndirectParams in render_features.h).
    // Trailing so no field above moves -- std140 only requires a matching prefix.
    // Read by pixel_forward_shading.glsl's gfx_pixel_forward_shade() via `lights.*`.
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;
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

// Set 7: material textures -- see engine::util::MaterialTextureCache, appended after every
// existing set (0-6) so none of their indices move. BLEND materials never alpha-test (see
// PushConstants.alpha_cutoff's doc below), so binding 0 (alpha mask) is intentionally left
// undeclared here even though the material set always binds it (a shader may leave a
// descriptor set's binding undeclared as long as it never samples it -- same reasoning as
// set 3 binding 0 above).
layout(set = 7, binding = 1) uniform sampler2D u_albedo_map;
layout(set = 7, binding = 2) uniform sampler2D u_normal_map;
layout(set = 7, binding = 3) uniform sampler2D u_metallic_roughness_map;

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
// stream -- see gfx/surface/transparent_vs.glsl, which this pass's vertex stage uses) and
// pushed once per draw by TransparentPass::push(). [32, 64) is the standard trailing
// "surface" block every surface-shader backbone appends (gfx_time/gfx_params -- see
// GBufferPipeline::PushConstants' doc; a derived transparent shader's fragment hook, e.g.
// water's rippled normals, reads gfx_params here), also part of TransparentPass::push().
// [64, 96) is a per-object refraction block (ior/thickness/tint/enabled) pushed once per
// draw by PixelRenderPipeline::record_transparent_() via TransparentPass's extra_pc_bytes
// ctor param -- GLSL permits only one push_constant block per stage, so all three regions
// live in this one struct despite coming from two separate push_constants() calls. The
// frame-level lighting/indirect/SSR block that used to occupy this region moved to set 6's
// UBO above (see TransparentRefractionPushConstants' doc for why).
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;  // unused here -- BLEND materials never alpha-test

    vec4  gfx_time;   // x=time, y=delta_time, z=frame_index, w=spare
    vec4  gfx_params; // four author-defined floats; see the surface shader's own doc

    vec4  refraction_tint_thickness; // rgb = tint, w = thickness
    vec4  refraction_ior_flags;      // x = ior, y = enabled (!= 0), zw reserved
} material;

// See gfx/surface/gbuffer_vs.glsl's identical aliases.
vec4 gfx_time   = material.gfx_time;
vec4 gfx_params = material.gfx_params;

layout(location = 0) out vec4 out_color;

/// What a fragment-shading hook may edit before lighting/SSR/refraction consume it --
/// deliberately just the normal (plus position/uv for context), unlike GfxSurface's fuller
/// albedo/metallic/roughness set: a derived TRANSPARENT shader (water) still wants the
/// exact same BRDF and refraction the backbone already computes, just fed a perturbed
/// normal, so the surface stays recognizably glass/water rather than becoming a different
/// material model per shader.
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

void main() {
    vec4 albedo_tex = texture(u_albedo_map, frag_uv);
    // glTF packing: metallic in B, roughness in G. Fallback is opaque white, so mr ==
    // vec2(1.0, 1.0) and the two lines below collapse to today's untextured values.
    vec2 mr = texture(u_metallic_roughness_map, frag_uv).bg;

    // Tangent-space normal map rotated into world space -- see gbuffer_fs.glsl's identical
    // derivation and its doc on why the flat-normal fallback round-trips to frag_world_normal
    // (up to ~0.32 degrees, not bit-identical).
    vec3 N = normalize(frag_TBN * (texture(u_normal_map, frag_uv).xyz * 2.0 - 1.0));
    if (!gl_FrontFacing) N = -N; // correct if cull_mode is ever relaxed to allow back faces

    GfxTransparentSurface s;
    s.normal_ws   = N;
    s.position_ws = frag_world_pos;
    s.uv          = frag_uv;
    gfx_surface_fragment(s);
    N = normalize(s.normal_ws);

    GfxForwardMaterial mat;
    mat.albedo    = material.albedo.rgb * albedo_tex.rgb;
    mat.alpha     = material.albedo.a * albedo_tex.a;
    mat.metallic  = material.metallic  * mr.x;
    mat.roughness = material.roughness * mr.y;
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
                                     camera.view, camera.proj, inverse(camera.proj), rmat, rp,
                                     forward_globals.ssr_mip.x);
}

#endif // GFX_SURFACE_TRANSPARENT_FS_GLSL
