#version 450
#define WATER_LOCAL_FRAME   // a local-frame body (see water_surface.glsl)
#define WATER_LAVA          // lava: the water surface as molten rock (see water_surface.glsl)
// Lava's evaluation stage -- the waves on every tessellated vertex, as water.tese does.
#define GFX_SURFACE_VERTEX
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_tes.glsl>
#include "water_surface.glsl"
