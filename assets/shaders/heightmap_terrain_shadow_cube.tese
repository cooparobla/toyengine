#version 450
// `heightmap_terrain` surface shader, cube shadow evaluation stage.
#define GFX_SURFACE_VERTEX
#define GFX_SHADOW_CUBE
#include <gfx/surface/shadow_tes.glsl>
#include "heightmap_terrain_surface.glsl"
