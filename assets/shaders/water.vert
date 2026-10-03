#version 450

// Water's forward-transparent vertex entry point: Gerstner waves and the flow/turbulence
// hand-off (see water_surface.glsl) over the transparent backbone. The SAME .spv this file
// compiles to is also used by TransparentCapturePass for water (see TransparentCapturePass::
// add_variant()'s doc) -- SSR must reflect the same displaced surface this file draws, which
// is also why every water fragment entry point declares GFX_SURFACE_CUSTOM_VARYING too.
#define GFX_SURFACE_VERTEX
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_vs.glsl>
#include "water_surface.glsl"
