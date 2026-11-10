#version 450

// World-space UI "text" fragment shader -- samples the bound texture's RED channel as alpha
// coverage (an R8 glyph atlas, see FontAtlas). Identical to ui_text.frag apart from the
// occlusion prologue; see ui_world_occlude.glsl.

// Quoted, not <angled>: glslc resolves a quoted include relative to the including file, so
// this needs no -I at all -- unlike ui.vert's <gfx/...> include, which depends on
// gfx_add_shader_target() supplying it.
#include "ui_world_occlude.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push {
    mat4  clip_from_canvas;
    vec2  inv_target_size;
    float occlude;
    float depth_bias;
} pc;

void main() {
    if (ui_world_occluded(pc.occlude, pc.inv_target_size, pc.depth_bias)) discard;
    float coverage = texture(tex, v_uv).r;
    out_color = vec4(v_color.rgb, v_color.a * coverage);
}
