#version 450

// Hair's G-buffer vertex entry point: the stock backbone plus the wind sway. See
// hair_surface.glsl for the sway itself, shared with the two shadow entry points.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>
#include "hair_surface.glsl"
