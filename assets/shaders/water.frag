#version 450

// Water's forward-transparent fragment entry point: animated ripple normals (see
// water_surface.glsl) over the transparent backbone -- everything else (BRDF, SSR,
// refraction, absorption) is the backbone's, unchanged. See water_capture.frag for the
// SSR-secondary-source capture's own entry point (same hook, fewer descriptor sets).
#define GFX_SURFACE_FRAGMENT
#include <gfx/surface/transparent_fs.glsl>
#include "water_surface.glsl"
