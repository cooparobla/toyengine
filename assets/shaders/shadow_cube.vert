#version 450

// Point light cubemap face vertex shader. Derived from
// blendy/assets/shaders/shadow_cube.vert; see shadow_depth.vert's comment
// on why `alpha` stays declared but unused.

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;      // only present when ShadowPipeline was built with a
                                          // material_layout (CUTOUT support) -- see shadow_pipeline.h
layout(location = 4) in mat4 in_model;   // per-instance (locations 4-7)

layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec2 frag_uv;

layout(push_constant) uniform CubeShadowPC {
    mat4 light_space_matrix;
    vec4 light_pos_range; // xyz = light pos, w = range
    float alpha;
    float alpha_cutoff; // 0.0 disables shadow_cube.frag's alpha-mask discard
} pc;

void main() {
    vec4 world_pos = in_model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;
    frag_uv        = in_uv;
    gl_Position    = pc.light_space_matrix * world_pos;
}
