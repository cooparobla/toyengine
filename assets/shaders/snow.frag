#version 450
// `snow` surface shader, G-buffer fragment stage: the snow's look and its height-field normal
// (snow_surface.glsl). The cover layer is off -- this shader draws the snow itself.
#define GFX_SURFACE_FRAGMENT
#define GFX_SURFACE_NO_SNOW
#include <gfx/texel_aa.glsl>
#define GFX_SURFACE_SAMPLE(tex, uv) texture(tex, gfx_texel_aa_uv(uv, vec2(textureSize(tex, 0))))
#include <gfx/surface/gbuffer_fs.glsl>
#include "snow_surface.glsl"
