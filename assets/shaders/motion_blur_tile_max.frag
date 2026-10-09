#version 450

// motion_blur_tile_max.frag -- motion blur stage 1 (toyengine/render/passes/motion_blur_pass.h).
//
// One fragment per tile: the longest blur half vector (pixels) among the tile's tile x tile pixels
// (McGuire et al. 2012, "TileMax"). Written as-is into an RG16F target at tile resolution.

#include "motion_blur_common.glsl"

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_max;

layout(set = 0, binding = 0) uniform sampler2D tex_velocity;   // G4, nearest

layout(push_constant) uniform TileParams {
    mat4  sky_reproject;
    vec4  extent_scale;   // xy = render extent, z = 0.5 * shutter, w = max radius (px)
    ivec4 info;           // x = tile size (px), y = sky reprojection valid
} pc;

void main() {
    const int   tile  = pc.info.x;
    const ivec2 size  = ivec2(pc.extent_scale.xy);
    const ivec2 base  = ivec2(gl_FragCoord.xy) * tile;
    const bool  sky_ok = pc.info.y != 0;

    vec2  best     = vec2(0.0);
    float best_len = 0.0;
    for (int y = 0; y < tile; ++y) {
        for (int x = 0; x < tile; ++x) {
            ivec2 p = min(base + ivec2(x, y), size - 1);
            vec2  v = mb_velocity_depth(tex_velocity, p, pc.extent_scale.xy, pc.sky_reproject,
                                        pc.extent_scale.z, pc.extent_scale.w, sky_ok).xy;
            float l = dot(v, v);
            if (l > best_len) { best_len = l; best = v; }
        }
    }
    out_max = vec4(best, 0.0, 1.0);
}
