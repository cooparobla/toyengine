#version 450

// Froxel-mode composite (FroxelVolumetricsPass's last stage): the raymarch composite's body
// with its upsample replaced by a lookup into the integrated froxel grid -- see
// gfx/volumetrics_composite_body.glsl. Binding 3 (march_result) carries the integrated atlas.

#define VOL_FROXEL_APPLY
#include <gfx/volumetrics_composite_body.glsl>
