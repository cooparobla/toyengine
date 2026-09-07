#version 450

// Water's forward-transparent vertex entry point: wave displacement (see
// water_surface.glsl) over the transparent backbone. The SAME .spv this file compiles to
// is also used by TransparentCapturePass for water (see TransparentCapturePass::
// add_variant()'s doc) -- SSR must reflect the same displaced surface this file draws.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/transparent_vs.glsl>
#include "water_surface.glsl"
