#version 450
// `heightmap_terrain` surface shader, G-buffer fragment stage: the grid's per-pixel normal, the
// eight splat-blended paint layers, rain and deep snow -- see heightmap_terrain_surface.glsl.
#define GFX_SURFACE_FRAGMENT
// This shader wets itself (heightmap_terrain_surface.glsl), with a puddle factor from the
// height grid's cavity -- data the backbone has no access to. Let it, or it soaks twice.
#define GFX_SURFACE_NO_WET
#include <gfx/surface/gbuffer_fs.glsl>
layout(set = 1, binding = 4) uniform sampler2D u_displacement;   // the height grid (R32F, metres)
#include "heightmap_terrain_surface.glsl"
