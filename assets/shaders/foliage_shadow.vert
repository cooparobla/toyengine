#version 450

// Foliage's directional-shadow vertex entry point -- the SAME displacement as
// foliage.vert, over the shadow backbone instead of the G-buffer one. See
// gfx/surface/shadow_vs.glsl's file doc for why this file must exist at all: a shadow
// entry point left at stock would give a caster that sways but a shadow that doesn't.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_vs.glsl>
// The sway reads the weather's wind (gfx_world.wind): the surface world at set 1.
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include "foliage_surface.glsl"
