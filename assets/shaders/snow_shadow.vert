#version 450
// `snow` surface shader, directional shadow vertex stage: the raised snow casts and receives
// the shadow it is drawn with.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_vs.glsl>
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
#include "snow_surface.glsl"
