#version 450

// Point-light cubemap-face variant of sdf_quad.vert -- see sdf_shadow.vert's
// doc. renderer_index sits after SdfCubeShadowPushConstants' mat4 (64 bytes)
// + vec4 (16 bytes) = offset 80.

layout(push_constant) uniform PC {
    layout(offset = 80) uint renderer_index;
} pc;

layout(location = 0) out flat uint frag_renderer_index;
layout(location = 1) out vec2 frag_ndc; // see sdf_quad.vert's doc

void main() {
    vec2 corners[6] = vec2[](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
    );
    vec2 ndc = corners[gl_VertexIndex];
    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_renderer_index = pc.renderer_index;
    frag_ndc = ndc;
}
