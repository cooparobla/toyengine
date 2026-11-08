#version 450

// planet_clouds' forward-transparent fragment entry point: a cloud coverage field over a sphere
// shell (see planet_clouds_surface.glsl), returned through GfxTransparentSurface::emission.
#define GFX_SURFACE_FRAGMENT
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_fs.glsl>
#include "planet_clouds_surface.glsl"
