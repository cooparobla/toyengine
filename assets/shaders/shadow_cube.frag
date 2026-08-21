#version 450

// Point light cubemap face fragment shader: linear depth normalized by the
// light's range. Derived from blendy/assets/shaders/shadow_cube.frag with
// the BLEND alpha-dither discard dropped (see shadow_depth.frag).

layout(location = 0) in vec3 frag_world_pos;

layout(push_constant) uniform CubeShadowPC {
    mat4 light_space_matrix;
    vec4 light_pos_range; // xyz = light pos, w = range
    float alpha;
} pc;

void main() {
    float light_dist = length(frag_world_pos - pc.light_pos_range.xyz);
    gl_FragDepth = clamp(light_dist / pc.light_pos_range.w, 0.0, 1.0);
}
