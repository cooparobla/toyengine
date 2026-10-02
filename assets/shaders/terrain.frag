#version 450

// `terrain` surface shader: the G-buffer fragment stage for greedy-meshed terrain chunks.
//
// A merged quad spans several tiles, but its atlas cell must still repeat once per tile. The
// mesher (toyengine/world/terrain_chunk.h's mesh_chunk_columns_greedy) therefore writes UVs in
// TILE space with the atlas cell index packed into u -- see TileMeshLibrary::encode_uv():
//
//     u = cell * 1024 + 512 + local_u,   v = local_v
//
// Every texture fetch the G-buffer body makes goes through GFX_SURFACE_SAMPLE, so redefining
// it here is the whole shader: decode, repeat the local position inside the cell, then apply
// the same texel-AA sharpening the stock gbuffer.frag does -- without it the bilinear sampler
// texel_aa selects would smear the 16-texel tiles into a blur.
//
// Atlas layout comes from the material's shader_params (filled in by TerrainSystem):
//   x = cell columns, y = cell rows, z = texels per cell edge (for the half-texel inset that
//   keeps a NEAREST/LINEAR tap from bleeding into the neighbouring cell -- atlas_cell() in
//   toyengine/world/tile_types.h uses the same inset).

vec4 terrain_sample(sampler2D tex, vec2 uv);
#define GFX_SURFACE_SAMPLE(tex, uv) terrain_sample(tex, uv)
#define GFX_SURFACE_SAMPLE_NORMAL(tex, uv) terrain_sample(tex, uv)
#include <gfx/surface/gbuffer_fs.glsl>

vec4 terrain_sample(sampler2D tex, vec2 uv) {
    const float stride = 1024.0;
    float cols   = max(gfx_params.x, 1.0);
    float rows   = max(gfx_params.y, 1.0);
    float texels = max(gfx_params.z, 1.0);

    float cell  = floor(uv.x / stride);
    vec2  local = vec2(uv.x - cell * stride - 0.5 * stride, uv.y);

    vec2 cell_size = vec2(1.0 / cols, 1.0 / rows);
    vec2 inset     = 0.5 / (vec2(cols, rows) * texels);
    vec2 origin    = vec2(mod(cell, cols), floor(cell / cols)) * cell_size + inset;
    vec2 extent    = cell_size - 2.0 * inset;

    // The UV a per-tile quad would have carried here -- exactly what the stock path samples.
    vec2 st = origin + fract(local) * extent;

    // Derivatives from the UNWRAPPED local coordinate: fract() jumps by 1 at every tile seam
    // inside a merged quad, and implicit derivatives across that jump would be huge there --
    // both for mip selection and for the texel-AA width below.
    vec2 dst_dx = dFdx(local) * extent;
    vec2 dst_dy = dFdy(local) * extent;

    // Texel-AA, as the stock gbuffer.frag applies through gfx_texel_aa_uv() (gfx/texel_aa.glsl):
    // snap to the texel centre except within one screen pixel of a texel edge, so the bilinear
    // sampler texel_aa selects gives hard pixel-art texels with a one-pixel anti-aliased edge.
    // Same arithmetic, with the screen-space width taken from the unwrapped derivatives.
    vec2 tex_size = vec2(textureSize(tex, 0));
    vec2 t = st * tex_size - 0.5;
    vec2 i = floor(t);
    vec2 f = t - i;
    vec2 w = clamp((abs(dst_dx) + abs(dst_dy)) * tex_size, 1e-6, 1.0);
    f = clamp((f - 0.5) / w + 0.5, 0.0, 1.0);
    vec2 aa_uv = (i + 0.5 + f) / tex_size;

    return textureGrad(tex, aa_uv, dst_dx, dst_dy);
}
