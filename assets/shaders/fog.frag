#version 450

// Global fog over the OPAQUE scene and sky (gfxcoopa's engine/passes/fog_pass.h). Runs after
// lighting/SSR and before any translucency is drawn: forward shaders (BLEND meshes, water,
// particles, SDF glass) fog themselves at their own distance (gfx_fog_eval in gfx/fog.glsl),
// so this pass only ever sees opaque geometry, whose distance is exactly the G-buffer's.
//
// Composites IN PLACE: the output is premultiplied (in-scatter, 1 - T) and the pipeline's
// premultiplied blend gives dst * T + in-scatter. The pass draws inside TransparentPass's
// render pass, which loads the live HDR image -- so it never samples the image it writes.
//
// Local volumes (fog pockets, wind, light shafts) are VolumetricsPass's, not this.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Set 0: G-buffer normal (sky test) and world position (fog distance), nearest-sampled --
// linear filtering would blend positions across silhouettes and fog every outline wrong.
layout(set = 0, binding = 0) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 1) uniform sampler2D g_position_roughness;

// Set 1: the light set -- its `fog` block carries every fog parameter (light_data.h).
#include <gfx/spot_light.glsl>
#include <light_ubo_body.glsl>

layout(push_constant) uniform FogPushConstants {
    mat4 inv_view_proj;  // unjittered clip -> world
    vec4 camera_pos;     // xyz
} pc;

#include <gfx/fog.glsl>

void main() {
    vec3 N      = texture(g_normal_metallic, in_uv).rgb;
    bool is_sky = dot(N, N) < 0.001;   // G1 normal is zero on sky
    vec3 cam    = pc.camera_pos.xyz;

    // The view ray from the matrix for EVERY pixel (NDC.y negated: this fullscreen triangle uses
    // a positive-height viewport while inv_view_proj follows the engine's Y-up convention), not
    // from G2 by subtraction -- a ray's direction is a property of the camera, and G2's
    // half-float positions lose precision at the large distances of grazing pixels.
    vec3 ndc   = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 world = pc.inv_view_proj * vec4(ndc, 1.0);
    vec3 dir   = normalize(world.xyz / world.w - cam);

    float dist = is_sky ? 0.0 : distance(cam, texture(g_position_roughness, in_uv).rgb);
    vec4  fog  = gfx_fog_eval(cam, dir, dist, is_sky, lights.fog,
                              lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
    out_color = vec4(fog.rgb, 1.0 - fog.a);
}
