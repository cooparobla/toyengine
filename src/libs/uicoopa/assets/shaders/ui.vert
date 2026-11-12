#version 450

// UI vertex shader -- shared by BOTH the "quad" (stock) and "text" pipeline variants (see
// ui_quad.frag/ui_text.frag): the position transform is identical either way, only the
// fragment stage differs in how it samples the bound texture. See
// gfx/surface2d/quad_vs.glsl for the shared affine-transform function this calls and the
// scale/offset convention it documents.

#include <gfx/surface2d/quad_vs.glsl>

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(push_constant) uniform Push {
    vec2 scale;  // = inv_canvas_size * 2 -- see UiPushConstants' own doc
    vec2 offset; // = vec2(-1.0) always, for uicoopa's bottom-left-origin canvas space --
                 // declared as a field (not folded into the shared function as a constant)
                 // so this block matches gfx_quad_2d_transform()'s (scale, offset) contract
                 // byte-for-byte, the same shape pixengine's sprite.vert also declares.
} pc;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = gfx_quad_2d_transform(in_pos, pc.scale, pc.offset);
}
