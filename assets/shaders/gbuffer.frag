#version 450

// Opaque/mask G-Buffer fragment shader. This app's material textures are
// pixel art (16-texel atlas cells, see toy::world::tile_types.h) bound with
// SamplerDesc::pixel_art_smooth()'s LINEAR sampler, and every material-map
// fetch is routed through gfx_texel_aa_uv(): texel interiors stay flat and
// hard-edged like point sampling, but a texel boundary blends across exactly
// one screen pixel instead of snapping to the pixel grid -- point sampling's
// snap is what made every magnified face crawl/shimmer whenever the camera
// moved sub-pixel. The backbone (gfx/surface/gbuffer_fs.glsl) is unchanged;
// GFX_SURFACE_SAMPLE is its sanctioned hook for exactly this.

#include <gfx/texel_aa.glsl>

#define GFX_SURFACE_SAMPLE(tex, uv) texture(tex, gfx_texel_aa_uv(uv, vec2(textureSize(tex, 0))))

#include <gfx/surface/gbuffer_fs.glsl>
