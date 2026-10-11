#version 450
// `heightmap_terrain` surface shader, G-buffer fragment stage: the grid's per-pixel normal, the
// eight splat-blended paint layers, rain and deep snow -- see heightmap_terrain_surface.glsl.
#define GFX_SURFACE_FRAGMENT
#include <gfx/surface/gbuffer_fs.glsl>
layout(set = 1, binding = 4) uniform sampler2D u_displacement;   // the height grid (R32F, metres)
#include "heightmap_terrain_surface.glsl"
