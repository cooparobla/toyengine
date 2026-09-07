#version 450

// Stock opaque/mask G-Buffer fragment shader -- no fragment-stage
// override. See gfx/surface/gbuffer_fs.glsl (gfxcoopa's base library) for
// the backbone this includes. This file is now byte-identical to
// gfxcoopa's own gbuffer.frag and is kept only so ShaderLibrary's
// app-over-base resolution stays explicit rather than silently falling
// through to the base copy.
#include <gfx/surface/gbuffer_fs.glsl>
