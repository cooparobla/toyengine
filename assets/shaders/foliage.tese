#version 450
// foliage.tese -- foliage's G-buffer evaluation stage: the wind sway (foliage_surface.glsl) on
// every tessellated vertex. See foliage.vert.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_tes.glsl>
#include "foliage_surface.glsl"
