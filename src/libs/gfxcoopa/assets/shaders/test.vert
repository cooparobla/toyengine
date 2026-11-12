#version 450

// Passthrough vertex shader — outputs clip-space position for a
// hard-coded screen-space triangle. No vertex buffer required.
// Used by the gfxcoopa test suite to exercise the full pipeline
// without needing asset loading.

vec2 positions[3] = vec2[](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

layout(location = 0) out vec3 frag_color;

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    frag_color  = colors[gl_VertexIndex];
}
