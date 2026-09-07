#version 450

// Point light cubemap face fragment shader: linear depth normalized by the
// light's range. Derived from blendy/assets/shaders/shadow_cube.frag with
// the BLEND alpha-dither discard dropped (see shadow_depth.frag). The
// alpha-mask discard is CUTOUT (AlphaMode::Mask) support -- see
// shadow_depth.frag's identical comment.

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec2 frag_uv;

layout(set = 0, binding = 0) uniform sampler2D u_alpha_mask;

layout(push_constant) uniform CubeShadowPC {
    mat4 light_space_matrix;
    vec4 light_pos_range; // xyz = light pos, w = range
    float alpha;
    float alpha_cutoff;
} pc;

void main() {
    if (pc.alpha_cutoff > 0.0 && texture(u_alpha_mask, frag_uv).a < pc.alpha_cutoff) discard;
    float light_dist = length(frag_world_pos - pc.light_pos_range.xyz);
    gl_FragDepth = clamp(light_dist / pc.light_pos_range.w, 0.0, 1.0);
}
