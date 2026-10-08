#version 450
// `snow` surface shader, G-buffer vertex stage: deep snow raises the surface (snow_surface.glsl).
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>
#include "snow_surface.glsl"
