#version 450

// Copies the cloud layer's resolved result into its history (SkyCloudPass), texel for texel:
// next frame's reconstruction (sky_cloud_resolve.frag) reads it while writing the result again.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_src;   // nearest

void main() {
    out_color = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0);
}
