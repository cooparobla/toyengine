#version 450

// Foliage's point-light cube-shadow vertex entry point -- same displacement again, over
// the cube shadow backbone. See foliage_shadow.vert's comment for why this file exists.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_cube_vs.glsl>
#include "foliage_surface.glsl"
