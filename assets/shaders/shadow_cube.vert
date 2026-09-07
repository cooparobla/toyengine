#version 450

// Point light cubemap face vertex shader -- identity displacement. See
// gfx/surface/shadow_cube_vs.glsl for the backbone this includes and the
// include-order contract a derived shader (e.g. foliage_shadow_cube.vert)
// must follow to keep displacement consistent with its G-Buffer entry
// point.
#include <gfx/surface/shadow_cube_vs.glsl>
