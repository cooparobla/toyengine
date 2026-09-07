#version 450

// Depth-only raymarch into one point-light cubemap face -- the SDF analogue
// of shadow_cube.frag. Writes the SAME linear light-distance-over-range
// depth convention shadow_cube.frag uses for meshes (not a standard
// perspective NDC depth), so the two share one shadow-sampling convention
// (gfx/shadow_sampling.glsl's gfx_shadow_cube_hard()) regardless of which
// caster wrote a given texel.

#include <gfx/sdf.glsl>

layout(location = 0) in flat uint frag_renderer_index;
layout(location = 1) in vec2 frag_ndc; // see sdf_quad.vert's doc

layout(push_constant) uniform PC {
    layout(offset = 0)  mat4 light_space_matrix;
    layout(offset = 64) vec4 light_pos_range; // xyz = light pos, w = range
    layout(offset = 80) uint renderer_index;
    layout(offset = 84) uint shadow_max_steps;
} pc;

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

    float light_dist = length(hit.pos - pc.light_pos_range.xyz);
    gl_FragDepth = clamp(light_dist / pc.light_pos_range.w, 0.0, 1.0);
}
