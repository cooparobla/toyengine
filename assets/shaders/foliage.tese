#version 450
// foliage.tese -- foliage's G-buffer evaluation stage: the wind sway (foliage_surface.glsl) on
// every tessellated vertex. See foliage.vert.
#define GFX_SURFACE_VERTEX
// See foliage.vert's GFX_SURFACE_PREV_POSE comment: the same true-velocity fix, here for the
// tessellated path.
#define GFX_SURFACE_PREV_POSE
#include <gfx/surface/gbuffer_tes.glsl>
#include "foliage_surface.glsl"
