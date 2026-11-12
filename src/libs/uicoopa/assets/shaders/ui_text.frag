#version 450

// UI "text" fragment shader -- samples the bound texture's RED channel as alpha coverage
// (an R8 glyph atlas, see FontAtlas) rather than a full RGBA color; the glyph's own color
// comes entirely from the per-vertex tint. See ui_quad.frag's doc for how the two
// pipeline variants are selected.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    float coverage = texture(tex, v_uv).r;
    out_color = vec4(v_color.rgb, v_color.a * coverage);
}
