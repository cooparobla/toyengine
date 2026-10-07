#version 450

// Copies the TAA resolve's colour into the shared AA output target.
//
// The resolve renders into TaaPass's own RGBA16F ping-pong target (whose alpha carries the
// per-pixel accumulation age, and whose colour precision the accumulation needs). Downstream
// consumers and the screenshot readback expect aa_target_'s RGBA8 with an opaque alpha, so
// this pass is the boundary: rgb through, alpha forced to 1.

layout(location = 0) in vec2 frag_uv;

layout(set = 0, binding = 0) uniform sampler2D tex_resolved;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(texture(tex_resolved, frag_uv).rgb, 1.0);
}
