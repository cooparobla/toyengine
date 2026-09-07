#version 450

// Foliage's G-buffer vertex entry point: identical to gbuffer.vert except for the
// displacement hook. See gfx/surface/gbuffer_vs.glsl for the include-order contract and
// foliage_surface.glsl for the actual wind-sway code shared by this file and
// foliage_shadow.vert/foliage_shadow_cube.vert.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>
#include "foliage_surface.glsl"
