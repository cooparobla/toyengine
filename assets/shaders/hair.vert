#version 450

// Hair's G-buffer vertex entry point: the stock backbone plus the wind sway. See
// hair_surface.glsl for the sway itself, shared with the two shadow entry points.
#define GFX_SURFACE_VERTEX
// Same true-velocity fix as foliage.vert: the sway is a function of the clock, so re-run the
// hook at last frame's pose for a real G-buffer velocity. NOT defined in hair_shadow.vert /
// hair_shadow_cube.vert -- shadow passes write no velocity.
#define GFX_SURFACE_PREV_POSE
#include <gfx/surface/gbuffer_vs.glsl>
#include "hair_surface.glsl"
