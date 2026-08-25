#version 450

// Blits the low-resolution LDR buffer to the swapchain. Derived from
// blendy/assets/shaders/blit.frag -- all the pixel-art work happens in the
// sampler (NEAREST) and the viewport (UpscalePass's letterbox rect), not in
// this shader.
//
// One addition beyond blit.frag: the swapchain's color attachment is
// VK_FORMAT_B8G8R8A8_SRGB (gfxcoopa/core/swapchain.h's choose_format()
// prefers it), so writing to it auto-encodes this shader's output from
// linear to sRGB -- correct for a physically-lit HDR pipeline (blendy's
// case), wrong here: post_target_ (VK_FORMAT_R8G8B8A8_UNORM, sampled below)
// already holds the exact display-referred bytes pixel_stylize.frag computed,
// including palette-quantized colors that must reach the screen unchanged.
// Left alone, the implicit encode brightens every pixel relative to those
// bytes. srgb_decode() predistorts so the hardware's encode cancels out and
// the window matches a headless screenshot of post_target_ exactly.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D color_sampler;

layout(location = 0) out vec4 out_color;

vec3 srgb_decode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void main() {
    out_color = vec4(srgb_decode(texture(color_sampler, in_uv).rgb), 1.0);
}
