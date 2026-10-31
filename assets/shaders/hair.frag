#version 450
// Hair's G-buffer fragment stage: the stock backbone plus the strand highlight.
//
// KEEP_BACKFACE_NORMAL because a strand already carries normals blown outward from its own
// axis (hair_data.cpp) -- flipping them on a back face would point them into the strand. Same
// reason foliage's cards keep theirs.
#define GFX_SURFACE_KEEP_BACKFACE_NORMAL
#define GFX_SURFACE_FRAGMENT
#include <gfx/surface/gbuffer_fs.glsl>
#include "hair_surface_frag.glsl"
