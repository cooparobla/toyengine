#version 450
// `snow` surface shader, G-buffer evaluation stage (tessellated renderers): the same raise per
// generated vertex, so trenches get the vertices to carve.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_tes.glsl>
#include "snow_surface.glsl"
