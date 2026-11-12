#version 450

// World-space UI vertex shader -- shared by BOTH the "quad" and "text" pipeline variants of
// UiWorldPass (see ui_world_quad.frag / ui_world_text.frag), exactly as ui.vert is for the
// screen-space UiPass.
//
// Deliberately does NOT #include <gfx/surface2d/quad_vs.glsl>: gfx_quad_2d_transform() is a
// 2D scale/offset into NDC that hardcodes gl_Position.z = 0 and applies its own Y flip. Both
// are wrong here. A world canvas needs a real z (so a depth compare means something) and
// must NOT flip Y itself -- the host renders through a negative-height viewport
// (VK_KHR_maintenance1), which is where this engine's Y flip already lives, and a second
// flip would mirror the whole canvas vertically.
//
// in_pos is unchanged canvas-space UiVertex data: canvas pixels, origin bottom-left, +Y up.
// clip_from_canvas carries the entire placement (proj * view * CanvasComponent::model()), so
// no widget, no DrawList and no vertex format had to learn anything about 3D.

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(push_constant) uniform Push {
    mat4  clip_from_canvas; // proj * view * model -- see CanvasComponent::model()
    vec2  inv_target_size;  // 1 / render-target extent, for the occlusion depth fetch
    float occlude;          // > 0.5 -> discard fragments behind opaque geometry
    float depth_bias;       // slack on that compare, in NDC depth units
} pc;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = pc.clip_from_canvas * vec4(in_pos, 0.0, 1.0);
}
