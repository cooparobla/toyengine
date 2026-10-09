#version 450

// The cloud march's 3D noise, baked once by SkyCloudPass into a 2D atlas: 64^3 texels packed as
// 8 x 8 slices of 64 x 64 (a 512 x 512 target). sky_clouds.frag samples it with hardware
// bilinear inside a slice and a manual lerp between slices (cloud_noise3d()).
//   r = one octave of value noise, lattice period 8 cells (the cloud's large lumps)
//   g = 3-octave value-noise fbm, lattice periods 4 / 8 / 16 cells (billows and wisps)
// Texel i of an axis sits at coordinate i / 63 of the period, so texel 63 repeats texel 0 and a
// clamped bilinear read inside one slice still wraps seamlessly (no gutter needed).

#include <gfx/noise.glsl>

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Value noise whose lattice wraps every `period` cells.
float tile_value_3d(vec3 p, float period) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    #define H(o) gfx_hash13(mod(i + o, period) + 17.0)
    return mix(mix(mix(H(vec3(0, 0, 0)), H(vec3(1, 0, 0)), f.x),
                   mix(H(vec3(0, 1, 0)), H(vec3(1, 1, 0)), f.x), f.y),
               mix(mix(H(vec3(0, 0, 1)), H(vec3(1, 0, 1)), f.x),
                   mix(H(vec3(0, 1, 1)), H(vec3(1, 1, 1)), f.x), f.y), f.z);
    #undef H
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 tile = px / 64;
    vec3 c = vec3(px % 64, tile.y * 8 + tile.x) / 63.0;   // [0, 1] over one period, per axis
    float lumps = tile_value_3d(c * 8.0, 8.0);
    float fbm = (tile_value_3d(c * 4.0 + 31.0, 4.0) + 0.5 * tile_value_3d(c * 8.0 + 57.0, 8.0)
               + 0.25 * tile_value_3d(c * 16.0 + 91.0, 16.0)) / 1.75;
    out_color = vec4(lumps, fbm, 0.0, 1.0);
}
