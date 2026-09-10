#version 450

// SSAO debug view: draws the SSAO pass's bound output (SsaoPass::output_view() when
// ssao_enabled, its neutral 1.0 texture otherwise -- see PixelRenderPipeline's ssao_view
// selection) fullscreen in place of lighting, so the occlusion buffer itself can be
// inspected directly instead of inferred from its (subtle) effect on ambient. Same shape
// as gbuffer_visualize.frag; splats .r to grayscale since the SSAO targets are single-
// channel VK_FORMAT_R8_UNORM (see ssao_pass.h), not RGB like the albedo attachment.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_ao;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(vec3(texture(u_ao, in_uv).r), 1.0);
}
