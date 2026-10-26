#version 450

// The `terrain_styled` surface shader: see terrain_styled.glsl (its body, shared with
// terrain_styled_planet.frag).
// terrain_styled wets itself with its own prefiltered blotch mask; see heightmap_terrain.frag.
#define GFX_SURFACE_NO_WET
#include "terrain_styled.glsl"
