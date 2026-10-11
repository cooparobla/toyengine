#version 450
// `heightmap_terrain` surface shader, directional shadow vertex stage: the caster is the surface
// the camera sees (heightmap_terrain_surface.glsl).
#define GFX_SURFACE_VERTEX
#include <gfx/surface/shadow_vs.glsl>
#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
layout(set = 0, binding = 4) uniform sampler2D u_displacement;   // the height grid
#include "heightmap_terrain_surface.glsl"
