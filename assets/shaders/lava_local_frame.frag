#version 450
#define WATER_LOCAL_FRAME   // a local-frame body (see water_surface.glsl)
#define WATER_LAVA          // lava: the water surface as molten rock (see water_surface.glsl)

// Lava's forward-transparent fragment entry point: a cooled crust where water would foam, and
// the heat between its plates written into GfxTransparentSurface::emission.
#define GFX_SURFACE_FRAGMENT
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_fs.glsl>
#include "water_surface.glsl"
