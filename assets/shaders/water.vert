#version 450

// Water's forward-transparent vertex entry point: Gerstner waves and the flow/turbulence
// hand-off (see water_surface.glsl) over the transparent backbone. The water fragment
// entry point declares GFX_SURFACE_CUSTOM_VARYING to match.
#define GFX_SURFACE_VERTEX
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_vs.glsl>
#include "water_surface.glsl"
