#version 450

// DebugLinePass's fragment stage -- flat per-vertex color, no lighting (gizmo overlay).

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color;
}
