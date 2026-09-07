#version 450

// Stock opaque/mask G-Buffer vertex shader -- identity displacement. See
// gfx/surface/gbuffer_vs.glsl (gfxcoopa's base library) for the backbone
// this includes and the include-order contract a derived shader (e.g.
// foliage.vert) must follow to override displacement while keeping
// everything else. This file is now byte-identical to gfxcoopa's own
// gbuffer.vert and is kept only so ShaderLibrary's app-over-base resolution
// stays explicit rather than silently falling through to the base copy.
#include <gfx/surface/gbuffer_vs.glsl>
