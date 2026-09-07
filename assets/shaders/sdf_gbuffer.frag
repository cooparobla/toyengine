#version 450

// Raymarches one OPAQUE/MASK SdfRenderer into the opaque G-buffer -- the SDF
// analogue of gbuffer.frag. Runs inside GBufferTarget's existing render pass
// (see SdfGBufferPass), scissored by the caller to the renderer's
// screen-space rectangle, so only pixels that can possibly hit this object
// ever reach the march below.

#include <gfx/sdf.glsl>

layout(location = 0) in flat uint frag_renderer_index;
layout(location = 1) in vec2 frag_ndc; // see sdf_quad.vert's doc

// Set 0: Camera UBO -- same layout gbuffer.vert/.frag already bind.
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: SdfData -- globals UBO (only inv_view_proj is read here; the
// lighting/SSR fields belong to the forward/capture passes) + renderer/shape
// SSBOs. See gfxcoopa/engine/data/sdf_data.h for the CPU-side layout.
layout(set = 1, binding = 0) uniform SdfGlobalsBlock {
    mat4  inv_view_proj;
    vec4  camera_pos;
    vec4  lighting0;
    vec4  lighting1;
    vec4  ssr0;
    vec4  ssr1;
    ivec4 ssr_steps;
    ivec4 ssr_mip;
} sdf_globals;
layout(std430, set = 1, binding = 1) readonly buffer SdfRendererBuffer { SdfRendererGpu sdf_renderers[]; };
layout(std430, set = 1, binding = 2) readonly buffer SdfShapeBuffer   { SdfShapeGpu sdf_shapes[]; };

#include <gfx/sdf_scene_body.glsl>

layout(location = 0) out vec4 out_albedo_ao;          // RGB = Albedo, A = AO
layout(location = 1) out vec4 out_normal_metallic;    // RGB = World Normal, A = Metallic
layout(location = 2) out vec4 out_position_roughness; // RGB = World Pos, A = Roughness
layout(location = 3) out vec4 out_emissive;           // RGB = emissive radiance (HDR), A = unused

void main() {
    SdfRendererGpu r = sdf_renderers[frag_renderer_index];

    // MASK test -- same all-or-nothing constant-alpha test gbuffer.frag applies, and cheaper to
    // reject here (before ever marching) since it never depends on the hit itself.
    if (r.mr_ao_cutoff.w > 0.0 && r.albedo_alpha.a < r.mr_ao_cutoff.w) discard;

    vec3 ro, rd;
    gfx_sdf_ray_from_clip(sdf_globals.inv_view_proj, frag_ndc, ro, rd);

    vec2 tbounds = gfx_sdf_aabb_intersect(ro, rd, r.bounds_min.xyz, r.bounds_max.xyz);
    if (tbounds.x > tbounds.y || tbounds.y < 0.0) discard;
    float t_start = max(tbounds.x, 0.0);

    GfxSdfHit hit = gfx_sdf_march_renderer(frag_renderer_index, ro, rd, t_start, tbounds.y,
                                           int(r.range.z), r.march.x);
    if (!hit.hit) discard;

    vec3 N = gfx_sdf_normal_renderer(frag_renderer_index, hit.pos, r.march.y);

    // Explicit depth write: the rasterized quad's own depth (see sdf_quad.vert) is meaningless --
    // this is the actual marched surface's depth, so opaque SDF geometry interleaves correctly
    // with rasterized mesh depth and is picked up by SSAO/outline/lighting/SSR/fog for free.
    vec4 clip = camera.proj * camera.view * vec4(hit.pos, 1.0);
    gl_FragDepth = clip.z / clip.w;

    out_albedo_ao          = vec4(r.albedo_alpha.rgb, r.mr_ao_cutoff.z);
    out_normal_metallic    = vec4(N, r.mr_ao_cutoff.x);
    out_position_roughness = vec4(hit.pos, r.mr_ao_cutoff.y);
    out_emissive            = vec4(r.emissive.rgb, 0.0);
}
