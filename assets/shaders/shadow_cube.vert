#version 450

// Point light cubemap face vertex shader. Derived from
// blendy/assets/shaders/shadow_cube.vert; see shadow_depth.vert's comment
// on why `alpha` stays declared but unused.

layout(location = 0) in vec3 in_position;
layout(location = 4) in mat4 in_model; // per-instance (locations 4-7)

layout(location = 0) out vec3 frag_world_pos;

layout(push_constant) uniform CubeShadowPC {
    mat4 light_space_matrix;
    vec4 light_pos_range; // xyz = light pos, w = range
    float alpha;
} pc;

void main() {
    vec4 world_pos = in_model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;
    gl_Position    = pc.light_space_matrix * world_pos;
}
