#ifndef GFX_VOLUMETRICS_FROXEL_INTEGRATE_BODY_GLSL
#define GFX_VOLUMETRICS_FROXEL_INTEGRATE_BODY_GLSL

// Froxel volumetrics, stage 2 of 3 (FroxelVolumetricsPass): front-to-back integration of
// the injected grid. One fragment per froxel; froxel (x, y, s) walks slices 0..s of its own
// column and writes the accumulated (in-scatter, transmittance) from the camera to the FAR
// edge of slice s -- what a pixel whose surface lies there sees through the medium. Stage 3
// then needs a single lookup per pixel.
//
// Each injected froxel is already integrated over its slice (see
// volumetrics_froxel_inject.frag), so composing slices is the raymarch's own step update:
// scatter += T * slice.rgb; T *= slice.a. That composition is associative, so the column
// walk is split in two passes over groups of kGroup slices -- ~9 taps per froxel instead of
// the ~D/2 a full walk averages:
//   VOL_FROXEL_PARTIAL defined: compose slices group_start..s of the INJECTED grid;
//   otherwise: compose the totals of all earlier groups (each group's last PARTIAL entry)
//              with this froxel's own partial.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_integrated;

// Set 0: the injected grid (partial pass) or the partial grid (final pass); texelFetch only.
layout(set = 0, binding = 0) uniform sampler2D source_grid;

const int kGroup = 8;

#include <gfx/volumetrics_ubo.glsl>
#include <gfx/volumetrics_froxel.glsl>

void main() {
    ivec2 W_H   = ivec2(u_vol.froxel_grid.xy);
    int   D     = int(u_vol.froxel_grid.z);
    int   cols  = int(u_vol.froxel_grid.w);
    ivec2 px    = ivec2(gl_FragCoord.xy);
    ivec2 tile  = px / W_H;
    int   slice = tile.y * cols + tile.x;
    ivec2 cell  = px - tile * W_H;
    if (slice >= D) {
        out_integrated = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3  scatter = vec3(0.0);
    float T       = 1.0;
    int   group_start = (slice / kGroup) * kGroup;
#ifdef VOL_FROXEL_PARTIAL
    for (int k = group_start; k <= slice; ++k) {
        vec4 f = texelFetch(source_grid, vol_froxel_tile_origin(k) + cell, 0);
        scatter += T * f.rgb;
        T       *= f.a;
    }
#else
    for (int g = kGroup - 1; g < group_start; g += kGroup) {   // each earlier group's total
        vec4 f = texelFetch(source_grid, vol_froxel_tile_origin(g) + cell, 0);
        scatter += T * f.rgb;
        T       *= f.a;
    }
    vec4 own = texelFetch(source_grid, vol_froxel_tile_origin(slice) + cell, 0);
    scatter += T * own.rgb;
    T       *= own.a;
#endif
    out_integrated = vec4(scatter, T);
}

#endif // GFX_VOLUMETRICS_FROXEL_INTEGRATE_BODY_GLSL
