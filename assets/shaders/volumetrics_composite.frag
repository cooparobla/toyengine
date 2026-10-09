#version 450

// Full-resolution composite of the LOCAL-volume march (volumetrics_march.frag) over the
// scene colour -- the second half of VolumetricsPass.
//
// The march runs at a fraction of this target's resolution, so the composite upsamples
// it with a joint-bilateral filter: of the four low-res samples around this pixel, each
// is weighted by its bilinear weight AND by how close the depth it was marched to is to
// this pixel's own depth. Without the depth term, a haze sample marched to a far wall
// would bleed across the silhouette of a near crate as a soft halo (and vice versa).
// The low-res depth needs no target of its own: the march read G2 at its own texel
// centres with a nearest sampler, so reading G2 at those same UVs here reproduces the
// exact distance each sample was marched to.
//
// Vertex stage is the shared fullscreen triangle (fullscreen.vert). Writes into its
// own HDR target -- pipeline::RenderPass hardcodes LOAD_OP_CLEAR, so this cannot
// composite in place onto the image it reads from.

#include <gfx/volumetrics_composite_body.glsl>
