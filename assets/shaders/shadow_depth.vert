#version 450

// Directional light depth map rendering -- identity displacement. See
// gfx/surface/shadow_vs.glsl for the backbone this includes and the
// include-order contract a derived shader (e.g. foliage_shadow.vert) must
// follow to keep displacement consistent with its G-Buffer entry point.
#include <gfx/surface/shadow_vs.glsl>
