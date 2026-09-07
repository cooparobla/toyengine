#version 450

// Depth-only raymarch into the directional shadow map -- the SDF analogue
// of shadow_depth.frag. Unlike the rasterized mesh path (which gets a
// correct depth for free from gl_Position's own interpolation),
// sdf_quad.vert's gl_Position.z is meaningless (see that file's doc), so
// this shader must explicitly march and write gl_FragDepth from the actual
// hit. Not scissored to a light-space rectangle -- see SdfShadowPass's file
// doc for why gfx_sdf_aabb_intersect()'s slab test is relied on instead.

#include <gfx/sdf.glsl>

layout(location = 0) in flat uint frag_renderer_index;
layout(location = 1) in vec2 frag_ndc; // see sdf_quad.vert's doc

// Only renderer_index/shadow_max_steps are read here -- light_space_matrix
// occupies [0, 64) but this stage only needs its INVERSE, computed once
// below rather than also passed as a second mat4 (would push this block
// over the 128-byte guaranteed push-constant minimum -- see
// SdfDirectionalShadowPushConstants's own doc).
layout(push_constant) uniform PC {
    layout(offset = 0)  mat4 light_space_matrix;
    layout(offset = 64) uint renderer_index;
    layout(offset = 68) uint shadow_max_steps;
} pc;

// Set 0: SdfData's renderer/shape SSBOs only -- no camera/light/globals UBO needed
// for a depth-only march driven entirely by the pushed light_space_matrix.
layout(std430, set = 0, binding = 1) readonly buffer SdfRendererBuffer { SdfRendererGpu sdf_renderers[]; };
layout(std430, set = 0, binding = 2) readonly buffer SdfShapeBuffer   { SdfShapeGpu sdf_shapes[]; };

#include <gfx/sdf_scene_body.glsl>

void main() {
    SdfRendererGpu r = sdf_renderers[frag_renderer_index];

    mat4 inv_light_space_matrix = inverse(pc.light_space_matrix);
    vec3 ro, rd;
    gfx_sdf_ray_from_clip(inv_light_space_matrix, frag_ndc, ro, rd);

    vec2 tbounds = gfx_sdf_aabb_intersect(ro, rd, r.bounds_min.xyz, r.bounds_max.xyz);
    if (tbounds.x > tbounds.y || tbounds.y < 0.0) discard;
    float t_start = max(tbounds.x, 0.0);

    int max_steps = min(int(r.range.z), int(pc.shadow_max_steps));
    GfxSdfHit hit = gfx_sdf_march_renderer(frag_renderer_index, ro, rd, t_start, tbounds.y,
                                           max_steps, r.march.x);
    if (!hit.hit) discard;

    vec4 clip = pc.light_space_matrix * vec4(hit.pos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
