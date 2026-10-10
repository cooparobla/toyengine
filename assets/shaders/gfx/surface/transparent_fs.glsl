#ifndef GFX_SURFACE_TRANSPARENT_FS_GLSL
#define GFX_SURFACE_TRANSPARENT_FS_GLSL

// gfx/surface/transparent_fs.glsl -- forward BLEND transparent fragment backbone.
//
// toyengine's own forward-shading pipeline (SSR trace inputs, forward_globals_ UBO,
// refraction). See gbuffer_fs.glsl for the include-order contract this follows;
// GFX_SURFACE_FRAGMENT gates gfx_surface_fragment() the same way GFX_SURFACE_VERTEX gates
// gfx_surface_vertex() in the *_vs.glsl backbones. The hook may perturb the world-space
// normal BEFORE lighting/SSR/refraction all consume it -- e.g. water's animated ripple
// normals -- but does not touch albedo/roughness/etc.; those stay backbone-owned so every
// derived transparent shader keeps the same BRDF and refraction behaviour.

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/ao_composite.glsl>
#include <gfx/ssr_common.glsl>
#include <gfx/spot_light.glsl>

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;
#ifdef GFX_SURFACE_CUSTOM_VARYING
layout(location = 6) in vec4 frag_custom; // see transparent_vs.glsl's frag_custom
#endif

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;   // z: pixel scale (0.5 * render height * proj[1][1]), for tessellation LOD
} camera;

// Set 1: Light UBO.
// The `lights` block (and the PointLight struct it needs) comes from the single copy in
// light_ubo_body.glsl -- see that file on why there is exactly one. Requires
// <gfx/spot_light.glsl>, included above.
#include <light_ubo_body.glsl>

// Set 2: Shadow maps -- one directional map, one point cube map, one spot map
// (see shadow_map_target.h). *Shadow: hardware compareEnable sampler
// (util::Sampler::shadow()) -- see gfx/shadow_sampling.glsl and
// toy_lighting.frag's identical binding for why.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform sampler2DShadow local_shadow_atlas; // point/spot shadows (gfx/local_shadow.glsl)
// The directional map AGAIN, through a plain nearest sampler: PCSS's blocker
// search needs stored depths, which a compare sampler cannot return.
layout(set = 2, binding = 3) uniform sampler2D dir_shadow_map_raw;
layout(set = 2, binding = 4) uniform sampler2D cloud_shadow_map;   // cloud_shadow.glsl

// Sets 3/4/5: ssr_pass_'s own trace-input sets (see toy_render_pipeline.h's
// transparent_extra ExtraSets), the same three sets ssr.frag itself binds at 1/2/3 --
// bound here at 3/4/5 since sets 0-2 above are this pass's own. Only the two bindings
// gfx/ssr_trace_body.glsl actually reads are declared (binding 0 of set 3, g_albedo_ao,
// is unused by the trace and is not declared here -- Vulkan permits a shader to leave
// bindings in its pipeline layout undeclared as long as it never samples them).
layout(set = 3, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 3, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 3, binding = 3) uniform sampler2D u_velocity; // G4: screen motion, for previous-frame hits
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
// instead (see the refraction plan for why SDF glass is excluded). A UBO rather than push
// constants -- see TransparentRefractionPushConstants' own doc (toy_render_pipeline.h):
// the frame block plus the per-object refraction fields would not fit in the 128-byte
// guaranteed Vulkan push-constant minimum.
#define WATER_MAX_RIPPLES 64 // == toy::render::kMaxWaterRipples (forward_globals.h)
layout(set = 6, binding = 0) uniform ForwardGlobalsBlock {
    vec4  lighting0;  // x=light_bands, y=spec_threshold, z=soft_lighting, w=rim_strength
    vec4  lighting1;  // x=ambient_intensity, y=sky_intensity, z=ssr_enabled, w=ssgi_intensity
    vec4  ssr0;       // x=ssgi_distance, y=ssr_max_distance, z=ssr_bias_texels, w=ssr_thickness_min
    vec4  ssr1;       // x=ssr_thickness_scale, y=ssr_roughness_cutoff, z=ssr_cone_prefilter
    ivec4 ssr_steps;  // x=ssr_max_iterations, y=ssr_max_hiz_mip, z=ssr_start_mip, w=ssr_min_mip0_steps
    ivec4 ssr_mip;    // x=ssr_max_color_mip, y=previous-frame colour flag
    vec4  refract0;   // x=enabled, y=strength, z=max_offset, w=chromatic
    vec4  refract1;   // x=blur, y=density, z=fresnel_enabled, w unused
    // Water ripple rings (toy::render::kMaxWaterRipples): info.x = count, .y = flow-ripple
    // layers, .z = detail distance (m), .w = ring draw range (m); per ring
    // [2i] = (x, y, age, strength), [2i+1] = (start radius, -, -, -). Read by water_surface.glsl.
    vec4  water_ripple_info;
    vec4  water_ripples[2 * WATER_MAX_RIPPLES];
} forward_globals;

#include <gfx/ssr_trace_body.glsl>
#include "indirect_hooks.glsl"
#include "toy_forward_shading.glsl"
#include "refraction.glsl"
#include <gfx/fog.glsl>

// Push constants: [0, 32) is the per-object material block, byte-identical to
// GBufferPipeline::PushConstants (model/normal_matrix moved to the per-instance vertex
// stream -- see gfx/surface/transparent_vs.glsl, which this pass's vertex stage uses) and
// pushed once per draw by TransparentPass::push(). [32, 64) is the standard trailing
// "surface" block every surface-shader backbone appends (gfx_time/gfx_params -- see
// GBufferPipeline::PushConstants' doc; a derived transparent shader's fragment hook, e.g.
// water's rippled normals, reads gfx_params here), also part of TransparentPass::push().
// [64, 96) is a per-object refraction block (ior/thickness/tint/enabled) pushed once per
// draw by ToyRenderPipeline::record_transparent_() via TransparentPass's extra_pc_bytes
// ctor param -- GLSL permits only one push_constant block per stage, so all three regions
// live in this one struct despite coming from two separate push_constants() calls. The
// frame-level lighting/indirect/SSR inputs are in set 6's UBO above (see
// TransparentRefractionPushConstants' doc for why).
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

    vec4  gfx_params_ext0;           // PBRMaterial::shader_params_ext[0] -- [96, 112)
    vec4  gfx_params_ext1;           // PBRMaterial::shader_params_ext[1] -- [112, 128)
} material;

// See gfx/surface/gbuffer_vs.glsl's identical aliases.
vec4 gfx_time        = material.gfx_time;
vec4 gfx_params      = material.gfx_params;
vec4 gfx_params_ext0 = material.gfx_params_ext0;
vec4 gfx_params_ext1 = material.gfx_params_ext1;

layout(location = 0) out vec4 out_color;

/// What a fragment-shading hook may edit before lighting/SSR/refraction consume it: the
/// normal plus the albedo/alpha/roughness/thickness inputs of the SAME BRDF and refraction the
/// backbone already computes -- so a derived TRANSPARENT shader (water) can perturb, tint and
/// fade the surface (ripples, foam, shoreline depth) while it stays recognizably glass/water
/// rather than becoming a different material model per shader. Metallic/AO stay the
/// material's.
struct GfxTransparentSurface {
    vec3  normal_ws;
    vec3  position_ws;
    vec2  uv;
    vec4  custom;     // frag_custom under GFX_SURFACE_CUSTOM_VARYING, else vec4(0)
    // Material terms, pre-filled from the material/textures and read back after the hook --
    // still the SAME BRDF/refraction model (a hook can tint, fade or roughen the surface,
    // e.g. water's foam and shoreline fade, but not swap the shading model out).
    vec3  albedo;
    float alpha;
    float roughness;
    float thickness;  // refraction/Beer-Lambert path length
    float ior;        // refraction IOR (outside / inside); water's underside inverts it. Capture: unused
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
#ifdef GFX_SURFACE_CUSTOM_VARYING
    s.custom      = frag_custom;
#else
    s.custom      = vec4(0.0);
#endif
    s.albedo      = material.albedo.rgb * albedo_tex.rgb;
    s.alpha       = material.albedo.a * albedo_tex.a;
    s.roughness   = material.roughness * mr.y;
    s.ior         = material.refraction_ior_flags.x;
    s.thickness   = material.refraction_tint_thickness.w;
    gfx_surface_fragment(s);
    N = normalize(s.normal_ws);

    GfxForwardMaterial mat;
    mat.albedo    = s.albedo;
    mat.alpha     = clamp(s.alpha, 0.0, 1.0);
    mat.metallic  = material.metallic  * mr.x;
    mat.roughness = clamp(s.roughness, 0.0, 1.0);
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
    p.ssr_cone_prefilter   = forward_globals.ssr1.z;
    p.ssr_prev_frame       = forward_globals.ssr_mip.y;

    vec4 shaded = toy_forward_shade(frag_world_pos, N, camera.camera_pos, camera.view, camera.proj, mat, p);

    vec3 V = normalize(camera.camera_pos - frag_world_pos);
    vec3 F0 = mix(vec3(0.04), mat.albedo, mat.metallic);

    GfxRefractionMaterial rmat;
    rmat.enabled   = material.refraction_ior_flags.y != 0.0;
    rmat.ior       = s.ior;
    rmat.thickness = max(s.thickness, 0.0);
    rmat.tint      = material.refraction_tint_thickness.rgb;

    GfxRefractionParams rp;
    rp.enabled         = forward_globals.refract0.x != 0.0;
    rp.strength        = forward_globals.refract0.y;
    rp.max_offset      = forward_globals.refract0.z;
    rp.chromatic       = forward_globals.refract0.w;
    rp.blur            = forward_globals.refract1.x;
    rp.density         = forward_globals.refract1.y;
    rp.fresnel_enabled = forward_globals.refract1.z != 0.0;

    // Global fog at THIS surface's distance (gfx/fog.glsl). Only the surface's own radiance is
    // fogged: the opaque scene behind it -- what the alpha blend shows through, and what
    // refraction samples -- was already fogged over its full distance by FogPass. Fogging
    // `shaded` before the composite covers both: gfx_refraction_apply() mixes it with the
    // transmitted background by alpha, exactly as the fixed-function blend does without
    // refraction. With the camera under water the in-water part of the ray is skipped (that is
    // UnderwaterPass's), so the surface seen from below stays unfogged.
    {
        float dist = length(frag_world_pos - camera.camera_pos);
        vec4  fog  = gfx_fog_eval(camera.camera_pos, (frag_world_pos - camera.camera_pos) / max(dist, 1e-6),
                                  dist, false, lights.fog,
                                  lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
        shaded.rgb = gfx_fog_composite(shaded.rgb, fog);
    }

    out_color = gfx_refraction_apply(shaded, frag_world_pos, N, V, mat.roughness, F0,
                                     camera.view, camera.proj, inverse(camera.proj), rmat, rp,
                                     forward_globals.ssr_mip.x);
}

#endif // GFX_SURFACE_TRANSPARENT_FS_GLSL
