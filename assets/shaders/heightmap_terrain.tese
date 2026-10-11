#version 450
// `heightmap_terrain` surface shader, G-buffer evaluation stage: each generated vertex placed on
// the height grid (gbuffer_tes.glsl declares u_displacement, the grid).
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_tes.glsl>
#include "heightmap_terrain_surface.glsl"
