#version 450
// Foliage's G-buffer fragment stage: the stock backbone, except that a card's back face keeps the
// card's (upward) normal instead of flipping it -- vegetation is lit the same from either side.
// Plain sampling (no pixel-art texel AA): foliage textures are ordinary photographic maps.
#define GFX_SURFACE_KEEP_BACKFACE_NORMAL
#include <gfx/surface/gbuffer_fs.glsl>
