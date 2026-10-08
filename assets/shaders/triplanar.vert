#version 450

// `triplanar` surface shader, vertex stage. Hands triplanar.frag the object-space position and
// normal plus the object's normal matrix, so its projection can run in either world or object
// (local) space -- see triplanar.frag for the mapping itself and the gfx_params layout.
//
// The three extra varyings sit after the backbone's frag_TBN (locations 3-5) and are matched
// one for one by triplanar.frag's inputs; only the G-buffer variant pairs these two stages.
// The hook lives in triplanar_surface.glsl, shared with triplanar.tese.

#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>
#include "triplanar_surface.glsl"
