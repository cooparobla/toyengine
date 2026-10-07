#version 450

// Water's forward-transparent fragment entry point: flow ripples, depth colour and foam (see
// water_surface.glsl) feeding the transparent backbone's own BRDF, SSR and refraction.
#define GFX_SURFACE_FRAGMENT
#define GFX_SURFACE_CUSTOM_VARYING
#include <gfx/surface/transparent_fs.glsl>
#include "water_surface.glsl"
