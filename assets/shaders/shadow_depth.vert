#version 450

// Directional light depth map rendering. Derived from
// blendy/assets/shaders/shadow_depth.vert with the BLEND-occluder alpha
// dither dropped (toyengine has no forward transparent pass) -- alpha stays
// declared, unused, only so this block matches ShadowPipeline's
// DirectionalShadowPushConstants byte-for-byte.

layout(location = 0) in vec3 in_position;
layout(location = 2) in vec2 in_uv;      // only present when ShadowPipeline was built with a
                                          // material_layout (CUTOUT support) -- see shadow_pipeline.h
layout(location = 4) in mat4 in_model;   // per-instance (locations 4-7)

layout(location = 0) out vec2 frag_uv;

layout(push_constant) uniform ShadowPC {
    mat4 light_space_matrix;
    float alpha;
    float alpha_cutoff; // 0.0 disables shadow_depth.frag's alpha-mask discard
} pc;

void main() {
    frag_uv = in_uv;
    gl_Position = pc.light_space_matrix * in_model * vec4(in_position, 1.0);
}
