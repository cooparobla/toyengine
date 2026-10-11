#version 450
// `heightmap_terrain` surface shader, cube shadow vertex stage (see heightmap_terrain_shadow.vert).
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_cube_vs.glsl>
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
layout(set = 0, binding = 4) uniform sampler2D u_displacement;   // the height grid
#include "heightmap_terrain_surface.glsl"
