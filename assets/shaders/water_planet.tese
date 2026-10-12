#version 450
#define WATER_LOCAL_FRAME   // water_planet: a local-frame body (see water_surface.glsl)
// water.tese -- water's evaluation stage: the Gerstner waves (water_surface.glsl) on every
// tessellated vertex, so near water gains wave detail the authored grid cannot hold. See water.vert.
#define GFX_SURFACE_VERTEX
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_tes.glsl>
#include "water_surface.glsl"
