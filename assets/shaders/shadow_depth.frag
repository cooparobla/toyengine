#version 450

// Directional light depth map rendering. Depth is written automatically by
// the fixed-function rasterizer -- no BLEND alpha-dither discard, unlike
// blendy's version, since toyengine has no forward transparent pass.

layout(push_constant) uniform ShadowPC {
    mat4 light_space_matrix;
    float alpha;
} pc;

void main() {
    // Empty: depth-only.
}
