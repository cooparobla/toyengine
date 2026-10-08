#version 450
// `snow` surface shader, cube shadow vertex stage (see snow_shadow.vert).
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_cube_vs.glsl>
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include "snow_surface.glsl"
