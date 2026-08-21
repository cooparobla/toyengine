#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer normal (used only to detect background pixels)
layout(set = 1, binding = 0) uniform sampler2D g_normal_metallic;

layout(push_constant) uniform SkyboxPushConstants {
    mat4 inv_view_proj;
} u_skybox;

#include "sky.glsl"

void main() {
    vec3 N = texture(g_normal_metallic, in_uv).rgb;
    if (dot(N, N) >= 0.001) {
        // Geometry present at this pixel; leave the lit color untouched.
        discard;
    }

    // Reconstruct a world-space view ray at the far plane for this pixel.
    // NDC.y is negated because this fullscreen triangle uses a positive-height
    // viewport (in_uv.y = 0 at the top row), while camera.proj follows the
    // Y-up OpenGL convention that ssr.frag/deferred lighting also unproject against.
    vec3 ndc = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 world = u_skybox.inv_view_proj * vec4(ndc, 1.0);
    vec3 dir = normalize(world.xyz / world.w - camera.camera_pos);

    out_color = vec4(sky_gradient(dir), 1.0);
}
