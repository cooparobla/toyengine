#version 450

// DebugLinePass's vertex stage -- see toyengine/render/passes/debug_line_pass.h. Draws pure
// world-space line segments (no model matrix; positions already come in world space from
// PhysicsWorld::debug_draw() or the editor), transformed by a push-constant view-projection.

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform DebugLinePC {
    mat4 view_proj;
    vec4 params;   // x: occlude, yz: 1 / target size, w: relative depth slack (fragment stage)
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    gl_Position = pc.view_proj * vec4(in_pos, 1.0);
    out_color = in_color;
}
