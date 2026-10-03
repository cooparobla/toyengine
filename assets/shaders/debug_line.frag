#version 450

// DebugLinePass's fragment stage -- flat per-vertex color, no lighting (gizmo overlay).
// Occluded lines (the editor grid) are discarded behind the G-buffer's scene depth.

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_scene_depth;

layout(push_constant) uniform DebugLinePC {
    mat4 view_proj;
    vec4 params;   // x: occlude, yz: 1 / target size, w: relative depth slack
} pc;

void main() {
    if (pc.params.x > 0.5) {
        // Same texel mapping as ui_world_occlude.glsl: drawn at the G-buffer's own extent, so
        // gl_FragCoord indexes it directly. Depth is standard [0,1] (Less), where 1 - z is
        // ~proportional to 1 / view distance -- so comparing (1 - z) with a relative slack
        // tolerates near-coplanar lines (grid on a floor) at any distance.
        float scene_z = texture(u_scene_depth, gl_FragCoord.xy * pc.params.yz).r;
        if ((1.0 - gl_FragCoord.z) < (1.0 - scene_z) * (1.0 - pc.params.w)) discard;
    }
    out_color = in_color;
}
