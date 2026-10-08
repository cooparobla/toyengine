#version 450
// water.tesc -- water's tessellation control stage: transparent.tesc with a cull margin wide
// enough for the Gerstner waves' horizontal and vertical reach (a patch just off-screen can
// swing into view).
#define GFX_TESS_CULL_MARGIN 4.0
#include <gfx/surface/transparent_tesc.glsl>
