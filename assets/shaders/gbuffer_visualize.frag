#version 450

// Temporary Phase-3 milestone shader: samples the G-buffer's albedo+ao
// attachment directly as final color, with no lighting applied, so geometry
// and the vertex-winding/Y-flip convention can be verified before
// pixel_lighting.frag exists. Superseded by pixel_lighting.frag in Phase 4.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D g_albedo_ao;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(texture(g_albedo_ao, in_uv).rgb, 1.0);
}
