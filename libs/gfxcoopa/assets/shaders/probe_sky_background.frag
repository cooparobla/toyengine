#version 450

// Fills the background of one reflection-probe capture face with the
// analytic sky, drawn FIRST (depth off) inside the same render-pass instance
// that then draws real scene geometry on top (depth on). RenderPass hardcodes
// LOAD_OP_CLEAR, so this can't be a separate pre-fill pass -- a second pass
// on the same attachment would just clear it away.

#include "cubemap_faces.glsl"
#include <gfx/sky.glsl>

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform SkyBgPC {
    int face; // cube array layer: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
} pc;

void main() {
    out_color = vec4(sky_gradient(cube_face_direction(pc.face, in_uv)), 1.0);
}
