#version 450

// Half-resolution SSAO's last stage (ssao_half_res): the blurred half-res AO upsampled to the
// G-buffer's resolution with the shared depth/normal-aware 2x2 filter
// (gfx/bilateral_upsample.glsl -- the same one the SSR composite uses), so the AO consumers
// (pixel_lighting.frag, the SSR composite) keep reading one full-resolution texture and an
// AO crease never bleeds across a silhouette into the surface behind it.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out float out_ao;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

layout(set = 1, binding = 0) uniform sampler2D u_ao_half;            // blurred, half resolution
layout(set = 1, binding = 1) uniform sampler2D g_normal_metallic;    // full resolution
layout(set = 1, binding = 2) uniform sampler2D g_position_roughness; // full resolution

#include <gfx/ssr_common.glsl>         // ssr_texel_world_size
#include <gfx/bilateral_upsample.glsl>

void main() {
    ivec2 gsize = textureSize(g_position_roughness, 0);
    ivec2 px    = clamp(ivec2(gl_FragCoord.xy), ivec2(0), gsize - 1);

    vec3 N = texelFetch(g_normal_metallic, px, 0).rgb;
    if (dot(N, N) < 0.001) {
        out_ao = 1.0;   // background: unoccluded, as the raw pass writes there
        return;
    }
    N = normalize(N);
    vec3  P        = texelFetch(g_position_roughness, px, 0).rgb;
    float view_z   = (camera.view * vec4(P, 1.0)).z;
    float px_world = ssr_texel_world_size(view_z, abs(camera.proj[1][1]), float(gsize.y));

    out_ao = gfx_bilateral_upsample(u_ao_half, g_position_roughness, g_normal_metallic, in_uv,
                                    vec2(textureSize(u_ao_half, 0)), vec2(gsize), P, N, px_world).r;
}
