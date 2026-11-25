#version 450

#define WATER_LAVA          // lava: the water surface as molten rock (see water_surface.glsl)

// Lava's forward-transparent vertex entry point. The same Gerstner displacement and flow
// hand-off water uses -- a body of lava is a body, and only its look and its numbers differ.
#define GFX_SURFACE_VERTEX
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_vs.glsl>
#include "water_surface.glsl"
