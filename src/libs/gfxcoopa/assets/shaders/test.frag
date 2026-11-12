#version 450

// Solid-color fragment shader for the gfxcoopa test suite.
// Receives interpolated RGB color from the vertex shader and
// outputs it as the final RGBA pixel value.

layout(location = 0) in  vec3 frag_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(frag_color, 1.0);
}
