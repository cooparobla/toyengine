#version 450

// Hair's point/spot cube-shadow vertex entry point -- the same sway again. See hair_shadow.vert.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_cube_vs.glsl>
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include "hair_surface.glsl"
