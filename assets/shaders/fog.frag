#version 450

// Fullscreen GLOBAL fog composite. Fog is global-only and config-driven; local
// volumes of every kind (including static fog pockets) are raymarched by
// VolumetricsPass instead -- see volumetrics_march.frag / VolumeComponent.
//
// Vertex stage is the shared fullscreen.vert triangle (see gfxcoopa's
// engine/passes/fog_pass.h). Reads the scene colour
// plus two G-buffer targets and writes a fogged copy into its own HDR
// target -- pipeline::RenderPass hardcodes LOAD_OP_CLEAR, so this cannot
// composite in place onto the image it reads from.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Set 0: scene colour (linear sampler) + G-buffer normal/position (nearest --
// linear filtering would blend world positions across silhouette edges and
// produce a wrong fog distance on every object outline).
layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;

layout(set = 1, binding = 0) uniform FogUBO {
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 fog_color;
    vec4 sun_direction;
    vec4 sun_color;
    vec4 mode_density;
    vec4 height_params;
    vec4 misc_params;
    vec4 sky_zenith;   // xyz used; see IndirectParams (render_features.h)
    vec4 sky_horizon;
    vec4 sky_ground;
} u_fog;

#include <gfx/fog.glsl>

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;

    vec3 N        = texture(g_normal_metallic, in_uv).rgb;
    vec3 cam_pos  = u_fog.camera_pos.xyz;
    bool is_sky   = dot(N, N) < 0.001;

    vec3  A = cam_pos;

    // Reconstruct the view ray precisely from inv_view_proj for EVERY pixel -- the same
    // convention pixel_lighting.frag's sky background uses (NDC.y negated: this fullscreen triangle uses a
    // positive-height viewport while inv_view_proj follows the Y-up convention every other
    // unprojection in this engine shares) -- rather than deriving it from the G2 world
    // position via subtraction on geometry pixels. The direction of a screen pixel's view
    // ray is a purely geometric property of the camera -- it doesn't depend on what's
    // rendered there -- so there is no reason to derive it two different ways (a precise
    // matrix unprojection for sky pixels vs. a subtraction against G2's quantized
    // R16G16B16A16_SFLOAT world position for geometry pixels, which loses real precision at
    // the large magnitudes a near-horizon/grazing-angle pixel has).
    vec3 ndc      = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 world    = u_fog.inv_view_proj * vec4(ndc, 1.0);
    vec3 view_dir = normalize(world.xyz / world.w - cam_pos);

    // The composite itself lives in gfx/fog.glsl, shared with volumetrics_composite.frag's
    // merged path (see gfx_fog_apply's doc) so the two cannot drift.
    float d_geo = is_sky ? 0.0 : distance(A, texture(g_position_roughness, in_uv).rgb);

    out_color = vec4(gfx_fog_apply(color, A, view_dir, d_geo, is_sky,
                                   u_fog.mode_density, u_fog.height_params, u_fog.misc_params,
                                   u_fog.fog_color.rgb, u_fog.sun_direction.xyz, u_fog.sun_color.rgb,
                                   u_fog.sky_zenith.rgb, u_fog.sky_horizon.rgb, u_fog.sky_ground.rgb),
                     1.0);
}
