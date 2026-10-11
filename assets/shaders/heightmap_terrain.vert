#version 450
// `heightmap_terrain` surface shader, G-buffer vertex stage (untessellated draws): the flat patch
// vertex placed on the height grid -- see heightmap_terrain_surface.glsl.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>
layout(set = 1, binding = 4) uniform sampler2D u_displacement;   // the height grid (R32F, metres)
#include "heightmap_terrain_surface.glsl"
