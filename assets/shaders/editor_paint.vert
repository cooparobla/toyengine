#version 450

// `editor_paint` surface shader, vertex stage: the editor's Vertex Paint / Weight Paint display.
//
// Editor only. The toyeditor's paint modes draw the painted mesh through an editor-built preview
// mesh (editor/app/paint_preview.h) that carries the colour to show -- the painted vertex colour,
// or the active vertex group's weight already mapped to a heatmap -- in slots the G-buffer
// backbone already interpolates, so no extra varying is needed (an extra one at location 6
// destabilized MoltenVK):
//   uv          = (red, green)
//   tangent.w   = 1 + blue   -- the backbone builds B = cross(N, T) * tangent.w, so the
//                               fragment stage reads blue back as length(B) - 1.
// Engine meshes never carry colour like this, so no scene material uses this shader.
//
// The tangent's xyz is rebuilt here as any direction perpendicular to the normal: the backbone
// normalizes T and builds the TBN from it, and a degenerate one would turn the frame NaN.

#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    vec3 ref = abs(v.normal_ws.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    v.tangent_ws = normalize(ref - v.normal_ws * dot(ref, v.normal_ws));
}
