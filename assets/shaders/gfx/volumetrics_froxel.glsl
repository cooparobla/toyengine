#ifndef GFX_VOLUMETRICS_FROXEL_GLSL
#define GFX_VOLUMETRICS_FROXEL_GLSL

// gfx/volumetrics_froxel.glsl -- froxel-grid addressing shared by FroxelVolumetricsPass's
// three stages (volumetrics_froxel_{inject,integrate,apply}.frag). Requires u_vol
// (gfx/volumetrics_ubo.glsl).
//
// The grid: W x H froxels per slice (each covering froxel_tile render pixels square),
// D depth slices along each pixel's VIEW RAY (distance from the camera, the same measure
// the raymarch uses), distributed exponentially between near and far so slices are thin
// up close and long far away -- the distribution Unreal and HDRP use.
//
// There are no 3D images in gfxcoopa, so the grid lives in a 2D ATLAS: slice s is a W x H
// tile at column s % cols, row s / cols (cols = froxel_grid.w).

/// Ray distance of slice BOUNDARY b (0..D): b = 0 is near, b = D is far.
float vol_froxel_boundary_dist(float b) {
    float near = u_vol.froxel_params.x;
    float far  = u_vol.froxel_params.y;
    return near * pow(far / near, b / u_vol.froxel_grid.z);
}

/// Inverse of vol_froxel_boundary_dist: fractional boundary coordinate in [0, D].
float vol_froxel_slice_of(float dist) {
    float near = u_vol.froxel_params.x;
    float far  = u_vol.froxel_params.y;
    float s = u_vol.froxel_grid.z * log(max(dist, near) / near) / log(far / near);
    return clamp(s, 0.0, u_vol.froxel_grid.z);
}

/// Top-left atlas texel of slice `slice`'s tile.
ivec2 vol_froxel_tile_origin(int slice) {
    int cols = int(u_vol.froxel_grid.w);
    return ivec2((slice % cols) * int(u_vol.froxel_grid.x), (slice / cols) * int(u_vol.froxel_grid.y));
}

/// Bilinear sample of one slice at screen uv, clamped to the slice's interior so the filter
/// never reaches into a neighbouring tile (a different depth). `atlas` must use a LINEAR sampler.
vec4 vol_froxel_sample(sampler2D atlas, vec2 uv, int slice) {
    vec2 wh    = u_vol.froxel_grid.xy;
    vec2 local = clamp(uv * wh, vec2(0.5), wh - 0.5);
    vec2 px    = vec2(vol_froxel_tile_origin(slice)) + local;
    return textureLod(atlas, px / vec2(textureSize(atlas, 0)), 0.0);
}

#endif // GFX_VOLUMETRICS_FROXEL_GLSL
