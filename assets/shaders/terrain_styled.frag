#version 450

// The `terrain_styled` surface shader: see terrain_styled.glsl, its body -- which a project can
// also build with TERRAIN_CURVED for a world whose up is not +Z.
// terrain_styled wets itself with its own prefiltered blotch mask; see heightmap_terrain.frag.
#define GFX_SURFACE_NO_WET
#include "terrain_styled.glsl"
