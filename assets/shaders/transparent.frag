#version 450

// Stock forward BLEND transparent fragment shader -- no fragment-stage override. See
// gfx/surface/transparent_fs.glsl for the backbone this includes and the include-order
// contract a derived shader (e.g. water.frag) must follow to perturb the normal while
// keeping everything else (BRDF, SSR, refraction).
#include <gfx/surface/transparent_fs.glsl>
