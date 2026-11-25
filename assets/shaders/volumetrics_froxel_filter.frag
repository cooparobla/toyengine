#version 450

// Froxel volumetrics, the optional stage between inject and integrate (FroxelVolumetricsPass):
// a small blur ACROSS each slice, the counterpart of HDRP's gaussian denoise of its volumetric
// buffer.
//
// What it is for: a froxel's value is one jittered sample averaged over time against its own
// history, so neighbouring froxels converge to the means of their own footprints. Where the
// medium changes sharply across a cell those means differ, and the grid's own 8-pixel cell
// structure survives into the composite's bilinear lookup -- measurable as a bump in the
// residual's autocorrelation at the froxel pitch. Averaging a froxel with its four neighbours
// in the SAME slice removes most of it for one pass over the atlas.
//
// Only in-slice (xy). Neighbouring SLICES are deliberately left alone: each froxel's value is
// already integrated over its own dt, and slices have different lengths, so blending across them
// would mix different path lengths rather than smooth one quantity.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_froxel;

// Set 0: the injected grid; texelFetch only.
layout(set = 0, binding = 0) uniform sampler2D source_grid;

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
        out_froxel = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    ivec2 origin = vol_froxel_tile_origin(slice);
    vec4  centre = texelFetch(source_grid, origin + cell, 0);

    // A cross, clamped to the tile: a neighbour outside this slice's own tile belongs to a
    // different depth, so the centre stands in for it rather than reaching across.
    const ivec2 kOffsets[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
    vec4 sum = centre * 0.5;
    for (int i = 0; i < 4; ++i) {
        ivec2 c = cell + kOffsets[i];
        bool inside = all(greaterThanEqual(c, ivec2(0))) && all(lessThan(c, W_H));
        sum += 0.125 * (inside ? texelFetch(source_grid, origin + c, 0) : centre);
    }
    out_froxel = sum;
}
