#version 450

// Foliage's G-buffer vertex entry point: identical to gbuffer.vert except for the
// displacement hook. See gfx/surface/gbuffer_vs.glsl for the include-order contract and
// foliage_surface.glsl for the actual wind-sway code shared by this file and
// foliage_shadow.vert/foliage_shadow_cube.vert.
#define GFX_SURFACE_VERTEX
// The sway is a function of the clock, so give the G-buffer a TRUE velocity for it by
// re-running the hook at last frame's pose -- see gbuffer_vs.glsl's GFX_SURFACE_PREV_POSE.
// NOT defined in foliage_shadow.vert/foliage_shadow_cube.vert: shadow passes write no velocity.
#define GFX_SURFACE_PREV_POSE
#include <gfx/surface/gbuffer_vs.glsl>
#include "foliage_surface.glsl"
