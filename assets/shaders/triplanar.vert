#version 450

// `triplanar` surface shader, vertex stage. Hands triplanar.frag the object-space position and
// normal plus the object's normal matrix, so its projection can run in either world or object
// (local) space -- see triplanar.frag for the mapping itself and the gfx_params layout.
//
// The three extra varyings sit after the backbone's frag_TBN (locations 3-5) and are matched
// one for one by triplanar.frag's inputs; only the G-buffer variant pairs these two stages.

#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>

layout(location = 6) out vec3 frag_object_pos;
layout(location = 7) out vec3 frag_object_normal;
layout(location = 8) flat out mat3 frag_normal_matrix; // locations 8-10

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    frag_object_pos    = v.position_os;
    frag_object_normal = v.normal_os;
    frag_normal_matrix = v.normal_matrix;
}
