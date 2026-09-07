#version 450

// Directional shadow variant of sdf_quad.vert. Same full-NDC quad; only the
// push-constant layout differs, since this pipeline's fragment stage also
// needs SdfDirectionalShadowPushConstants::light_space_matrix (see
// sdf_shadow.frag) ahead of renderer_index in the shared push-constant
// block -- this stage skips straight to its own field via an explicit
// `layout(offset = ...)`, which GLSL permits without redeclaring the
// leading mat4 it never reads.

layout(push_constant) uniform PC {
    layout(offset = 64) uint renderer_index;
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
