#version 450

// Hair's directional-shadow vertex entry point -- the SAME sway as hair.vert, over the shadow
// backbone. Without this file a groom would sway while its shadow stood still.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_vs.glsl>
// The sway reads the weather's wind (gfx_world.wind): the surface world at set 1 here.
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include "hair_surface.glsl"
