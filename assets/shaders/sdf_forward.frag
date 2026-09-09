#version 450

// Raymarches one BLEND SdfRenderer, lit and SSR-tracing, into the live HDR
// colour image -- the SDF analogue of transparent.frag, sharing its exact
// shading formula via pixel_forward_shading.glsl's gfx_pixel_forward_shade()
// (see that file's doc). Runs inside TransparentPass's own render pass (see
// SdfForwardPass), interleaved with BLEND meshes in one back-to-front sorted
// draw list (PixelRenderPipeline::record_transparent_()).

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/ssr_common.glsl>
#include <gfx/sdf.glsl>

layout(location = 0) in flat uint frag_renderer_index;
layout(location = 1) in vec2 frag_ndc; // see sdf_quad.vert's doc

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- identical layout to transparent.frag's
struct PointLight {
    vec4 position_range;  // xyz = pos, w = range
    vec4 color_intensity; // xyz = color, w = intensity
    vec4 attenuation;     // x=const, y=lin, z=quad, w=cast_shadows (1 or 0)
};

layout(set = 1, binding = 0) uniform LightUBO {
    vec4 dir_direction;
    vec4 dir_color;
    vec4 _reserved_was_dir_ambient; // was dir_ambient; see LightUBO's C++ doc (light_data.h)
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=unused, z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];

    // Configurable sky/ambient colour (see IndirectParams in render_features.h).
    // Trailing so no field above moves -- std140 only requires a matching prefix.
    // Read by pixel_forward_shading.glsl's gfx_pixel_forward_shade() via `lights.*`.
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;
} lights;

// Set 2: Shadow maps
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCubeShadow point_shadow_map;

// Set 3: SdfData -- globals UBO + renderer/shape SSBOs (see
// gfxcoopa/engine/data/sdf_data.h). Field order in `SdfGlobalsBlock` matches
// SdfGlobals's C++ layout exactly.
layout(set = 3, binding = 0) uniform SdfGlobalsBlock {
    mat4  inv_view_proj;
    vec4  camera_pos;
    vec4  lighting0;   // x=light_bands, y=spec_threshold, z=soft_lighting, w=rim_strength
    vec4  lighting1;   // x=ambient_intensity, y=sky_intensity, z=ssr_enabled, w=ssgi_intensity
    vec4  ssr0;        // x=ssgi_distance, y=ssr_max_distance, z=ssr_bias_texels, w=ssr_thickness_min
    vec4  ssr1;        // x=ssr_thickness_scale, y=ssr_roughness_cutoff
    ivec4 ssr_steps;   // x=ssr_max_iterations, y=ssr_max_hiz_mip, z=ssr_start_mip, w=ssr_min_mip0_steps
    ivec4 ssr_mip;     // x=ssr_max_color_mip
} sdf_globals;
layout(std430, set = 3, binding = 1) readonly buffer SdfRendererBuffer { SdfRendererGpu sdf_renderers[]; };
layout(std430, set = 3, binding = 2) readonly buffer SdfShapeBuffer   { SdfShapeGpu sdf_shapes[]; };

// Sets 4/5/6: ssr_pass_'s own trace-input sets, bound via SdfForwardPass's ExtraSets -- one
// higher index than transparent.frag's own 3/4/5, since this pass's own SdfData set occupies 3.
layout(set = 4, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 4, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 5, binding = 0) uniform sampler2D u_hiz_map;
layout(set = 6, binding = 0) uniform sampler2D u_scene_color;

#include <gfx/ssr_trace_body.glsl>
#include "indirect_hooks.glsl"
#include "pixel_forward_shading.glsl"
#include <gfx/sdf_scene_body.glsl>

layout(location = 0) out vec4 out_color;

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
    // Raymarched analogue of gl_FrontFacing's flip in transparent.frag: if the estimated
    // normal points the same way as the view ray, we're seeing the surface from "inside".
    if (dot(N, rd) > 0.0) N = -N;

    // Depth test against the opaque G-buffer (SdfForwardPass's depth.test=true,
    // depth.write=false -- never occlude other transparents, same as TransparentPass).
    vec4 clip = camera.proj * camera.view * vec4(hit.pos, 1.0);
    gl_FragDepth = clip.z / clip.w;

    GfxForwardMaterial mat;
    mat.albedo    = r.albedo_alpha.rgb;
    mat.alpha     = r.albedo_alpha.a;
    mat.metallic  = r.mr_ao_cutoff.x;
    mat.roughness = r.mr_ao_cutoff.y;
    mat.ao        = r.mr_ao_cutoff.z;

    GfxForwardLightingParams p;
    p.light_bands          = sdf_globals.lighting0.x;
    p.spec_threshold       = sdf_globals.lighting0.y;
    p.soft_lighting        = sdf_globals.lighting0.z;
    p.rim_strength         = sdf_globals.lighting0.w;
    p.ambient_intensity    = sdf_globals.lighting1.x;
    p.sky_intensity        = sdf_globals.lighting1.y;
    p.ssr_enabled          = sdf_globals.lighting1.z;
    p.ssgi_intensity       = sdf_globals.lighting1.w;
    p.ssgi_distance        = sdf_globals.ssr0.x;
    p.ssr_max_distance     = sdf_globals.ssr0.y;
    p.ssr_bias_texels      = sdf_globals.ssr0.z;
    p.ssr_thickness_min    = sdf_globals.ssr0.w;
    p.ssr_thickness_scale  = sdf_globals.ssr1.x;
    p.ssr_roughness_cutoff = sdf_globals.ssr1.y;
    p.ssr_max_iterations   = sdf_globals.ssr_steps.x;
    p.ssr_max_hiz_mip      = sdf_globals.ssr_steps.y;
    p.ssr_start_mip        = sdf_globals.ssr_steps.z;
    p.ssr_min_mip0_steps   = sdf_globals.ssr_steps.w;
    p.ssr_max_color_mip    = sdf_globals.ssr_mip.x;

    out_color = gfx_pixel_forward_shade(hit.pos, N, camera.camera_pos, camera.view, camera.proj, mat, p);
}
