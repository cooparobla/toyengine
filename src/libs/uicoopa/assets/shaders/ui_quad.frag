#version 450

// UI "quad" fragment shader -- the stock variant, RGBA-sampling the bound texture (a
// sprite, a nine-slice panel, or the 1x1 white fallback for an untextured Graphic). See
// ui_text.frag for the R8-coverage variant. The two are separate pipeline variants (see
// TexturedQuad2DPass::add_variant() and UiPass's ctor), selected per batch with a
// pipeline bind rather than a runtime branch on a push constant.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    out_color = texture(tex, v_uv) * v_color;
}
