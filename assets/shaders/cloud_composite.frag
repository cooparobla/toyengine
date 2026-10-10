#version 450

// The volumetric cloud layer over GEOMETRY pixels (CloudOverlayPass): a cloud between a high
// camera and the ground, or in front of a mountain. Drawn over the lit, fogged and translucent
// scene with premultiplied alpha (rgb = in-scattered light, a = 1 - transmittance). Sky pixels
// are left alone: the lighting pass already composited the layer over the sky (sky_physical.glsl).
//
// A depth-aware upsample of the half-resolution layer (sky_cloud_resolve.frag): each tap packs
// the clouds' distance (cloud space, sky_cloud_common.glsl), and a tap whose clouds lie behind this
// pixel's surface is dropped -- so the layer never bleeds onto a tower that rises into it from
// the blocks around its silhouette, which were marched to the ground behind.

#include "sky_cloud_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D g_normal;     // nearest
layout(set = 0, binding = 1) uniform sampler2D g_position;   // nearest: world position
layout(set = 0, binding = 2) uniform sampler2D u_clouds;     // nearest: the reconstructed layer

layout(push_constant) uniform CompositeParams {
    vec4 camera;   // xyz = camera position (world), w = cloud scale (world m per cloud m)
    vec4 region;   // x = the fraction of the cloud target the march filled
} pc;

void main() {
    vec3 N = texture(g_normal, in_uv).rgb;
    if (dot(N, N) < 0.001) discard;
    float surface = distance(texture(g_position, in_uv).xyz, pc.camera.xyz);

    ivec2 tsize = textureSize(u_clouds, 0);
    ivec2 size = clamp(ivec2(floor(vec2(tsize) * pc.region.x + 0.5)), ivec2(1), tsize);
    vec2 p = in_uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y <= 1; ++y) {
        for (int x = 0; x <= 1; ++x) {
            ivec2 q = clamp(i + ivec2(x, y), ivec2(0), size - 1);
            vec4 s = texelFetch(u_clouds, q, 0);
            if (s.a < 0.0) continue;
            float trans = cloud_trans(s.a);
            // Clouds behind the surface: not this pixel's (a clear texel holds no depth to test).
            if (trans < 0.999 && cloud_depth(s.a) * pc.camera.w > surface * 1.02 + 0.25) continue;
            float w = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y) + 1e-4;
            sum += vec4(s.rgb, trans) * w;
            wsum += w;
        }
    }
    if (wsum <= 0.0) discard;
    vec4 c = sum / wsum;
    float alpha = 1.0 - c.a;
    if (alpha <= 1e-4) discard;
    out_color = vec4(c.rgb, alpha);
}
