#ifndef GFX_TEXEL_AA_GLSL
#define GFX_TEXEL_AA_GLSL

// Texel-AA ("anti-aliased point sampling") for pixel-art textures under a free 3D
// camera. Requires the texture to be bound with a LINEAR sampler
// (SamplerDesc::pixel_art_smooth()); with a NEAREST sampler the sharpened UV still
// snaps and this is a no-op in look.

/// Sharpens a UV so bilinear interpolation happens only inside a one-screen-pixel
/// band at each texel boundary: texel interiors sample flat (the crisp fat-texel
/// look point sampling gives), while a boundary crossing a screen pixel blends over
/// exactly that pixel instead of flipping it. Point-sampled magnified texels snap
/// their edges to the screen grid every frame the camera moves sub-pixel -- on
/// high-contrast per-texel art that snap IS the full-surface crawl/shimmer.
///
/// At minification (texel footprint >= one screen pixel) the width clamp turns this
/// into plain bilinear, which also sizzles less than point sampling at distance.
///
/// @param uv       The interpolated texture coordinate.
/// @param tex_size The texture's dimensions in texels (vec2(textureSize(tex, 0))).
/// @return The sharpened coordinate to pass to texture().
vec2 gfx_texel_aa_uv(vec2 uv, vec2 tex_size) {
    vec2 t = uv * tex_size - 0.5;
    vec2 i = floor(t);
    vec2 f = t - i;
    vec2 w = clamp(fwidth(t), 1e-6, 1.0);
    f = clamp((f - 0.5) / w + 0.5, 0.0, 1.0);
    return (i + 0.5 + f) / tex_size;
}

#endif // GFX_TEXEL_AA_GLSL
