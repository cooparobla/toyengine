#version 450

// Foliage's point-light cube-shadow vertex entry point -- same displacement again, over
// the cube shadow backbone. See foliage_shadow.vert's comment for why this file exists.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_cube_vs.glsl>
// The sway reads the weather's wind (gfx_world.wind): the surface world at set 1.
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include "foliage_surface.glsl"
