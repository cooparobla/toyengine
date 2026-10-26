#version 450

// The `terrain_styled_planet` surface shader: terrain_styled for a planet TileTerrain's chunks
// (toyengine/world/README.md, Planets) -- see terrain_styled.glsl.
#define TERRAIN_PLANET
// As terrain_styled.frag: it wets itself.
#define GFX_SURFACE_NO_WET
#include "terrain_styled.glsl"
