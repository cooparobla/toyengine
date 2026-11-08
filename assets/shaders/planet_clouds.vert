#version 450

// planet_clouds' forward-transparent vertex entry point: passes the shell's own object-space
// position through to the fragment stage (see planet_clouds_surface.glsl) so the coverage noise
// rotates with the mesh instead of swimming in world space.
#define GFX_SURFACE_VERTEX
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_vs.glsl>
#include "planet_clouds_surface.glsl"
