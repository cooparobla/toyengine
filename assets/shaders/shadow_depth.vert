#version 450

// Directional light depth map rendering. Derived from
// blendy/assets/shaders/shadow_depth.vert with the BLEND-occluder alpha
// dither dropped (toyengine has no forward transparent pass) -- alpha stays
// declared, unused, only so this block matches ShadowPipeline's
// DirectionalShadowPushConstants byte-for-byte.

layout(location = 0) in vec3 in_position;
layout(location = 4) in mat4 in_model; // per-instance (locations 4-7)

layout(push_constant) uniform ShadowPC {
    mat4 light_space_matrix;
    float alpha;
} pc;

void main() {
    gl_Position = pc.light_space_matrix * in_model * vec4(in_position, 1.0);
}
