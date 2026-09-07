#version 450

// Water's SSR-secondary-source capture fragment entry point: the SAME normal-perturbation
// hook as water.frag (water_surface.glsl's gfx_surface_fragment()), but over the capture
// backbone (fewer descriptor sets: no SSR trace inputs, no forward_globals UBO -- see
// gfx/surface/capture_fs.glsl's doc) rather than the full transparent one. Deliberately a
// separate file from water.frag, not a reuse of it: TransparentCapturePass's pipeline
// layout only has sets 0-2, so a fragment shader declaring transparent_fs.glsl's sets 3-6
// would exceed it.
#define GFX_SURFACE_FRAGMENT
#include <gfx/surface/capture_fs.glsl>
#include "water_surface.glsl"
