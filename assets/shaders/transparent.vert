#version 450

// Stock forward-transparent vertex shader -- identity displacement on the
// gfx/surface/transparent_vs.glsl backbone, so a derived shader (e.g. water.vert) overrides
// displacement through the same hook mechanism every other pass uses.
#include <gfx/surface/transparent_vs.glsl>
