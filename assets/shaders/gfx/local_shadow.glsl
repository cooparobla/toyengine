#ifndef GFX_LOCAL_SHADOW_GLSL
#define GFX_LOCAL_SHADOW_GLSL

// gfx/local_shadow.glsl -- sampling the local-light (point/spot) shadow atlas.
//
// Every point and spot shadow of the frame lives in ONE depth atlas: a spot is a single tile, a
// point light a 3x2 block of six cube-face tiles. Faces are rendered with a field of view
// slightly wider than 90 degrees, so each tile carries a guard band of `pcf.z` texels that a
// PCF kernel can read without crossing into a neighbouring face -- the reason no cube-map
// sampling (and no gl_FragDepth distance write, which disabled early-Z) is needed.
//
// REQUIRED BEFORE INCLUDE:
//   - #define GFX_LOCAL_SHADOWS <expr>  -- the GfxLocalShadowBlock to read (e.g.
//     `lights.local_shadows` or `u_vol.local_shadows`), declared via gfx/local_shadow_types.glsl.
//   - `local_shadow_atlas`              -- sampler2DShadow over the atlas, compare op GREATER
//     (so a tap returns OCCLUSION, 1 = shadowed), linear filter (hardware 2x2 PCF per tap).

/// The cube face (0..5 = +X,-X,+Y,-Y,+Z,-Z) a light-to-point direction falls in, by major axis.
/// Must match the CPU's face order (toyengine's local shadow view matrices).
int gfx_local_shadow_face(vec3 d) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0.0 ? 0 : 1;
    if (a.y >= a.z)               return d.y >= 0.0 ? 2 : 3;
    return d.z >= 0.0 ? 4 : 5;
}

/// Atlas uv of view `face`'s tile origin within shadow `sh`.
vec2 gfx_local_shadow_tile_origin(GfxLocalShadow sh, int face) {
    return sh.tile.xy + vec2(float(face % 3), float(face / 3)) * sh.tile.z;
}

/// World-space depth bias -> this view's [0,1] perspective (RH_ZO) depth at view depth `w`:
/// d(ndc_z)/d(w) = far * near / ((far - near) * w^2).
float gfx_local_shadow_depth_bias(GfxLocalShadow sh, float bias_world, float w) {
    float n = sh.proj.x, f = sh.light.w;
    return bias_world * f * n / max((f - n) * w * w, 1e-12);
}

/// Occlusion (0 = lit, 1 = fully shadowed, scaled by the light's darkness) of `world_pos` from
/// local shadow `slot` (0-based), with normal-offset and texel-scaled constant + slope depth
/// bias, filtered by a rotated Vogel-disk PCF of `taps` hardware-compare taps.
///
/// Spot penumbrae are a constant WORLD width (pcf.x), converted per pixel against the texel
/// size at this depth, like the directional map's; point penumbrae are a constant texel radius
/// (pcf.y). Either is capped at the guard band (pcf.z) and every tap is clamped inside its
/// own tile, so no kernel ever reads a neighbouring light's depths.
float gfx_local_shadow(int slot, vec3 world_pos, vec3 N, vec3 L, float rotation, int taps) {
    GfxLocalShadow sh = GFX_LOCAL_SHADOWS.shadows[slot];
    int kind = int(sh.tile.w + 0.5);
    if (kind == 0) return 0.0;

    int  face = (kind == 2) ? gfx_local_shadow_face(world_pos - sh.light.xyz) : 0;
    mat4 m    = GFX_LOCAL_SHADOWS.view_proj[int(sh.proj.w + 0.5) + face];

    // View depth of the unbiased point -> this map's world size per texel there.
    vec4 clip0 = m * vec4(world_pos, 1.0);
    if (clip0.w <= 1e-4) return 0.0;
    float texel_world = 2.0 * clip0.w * sh.proj.y / max(sh.proj.z, 1.0);

    float radius = (kind == 1) ? (sh.pcf.x > 0.0 ? sh.pcf.x / max(texel_world, 1e-6) : 0.0)
                               : sh.pcf.y;
    radius = min(radius, sh.pcf.z);

    // Normal offset: a texel-scaled push off the surface that clears the PCF kernel too,
    // growing as the surface turns away from the light (where one texel covers the most of it).
    float ndl = max(dot(N, L), 0.0);
    vec3 biased = world_pos + N * (texel_world * (GFX_LOCAL_SHADOWS.info.z + radius) *
                                   (0.5 + 0.5 * (1.0 - ndl)));
    vec4 clip = m * vec4(biased, 1.0);
    if (clip.w <= 1e-4) return 0.0;
    vec3 ndc = clip.xyz / clip.w;
    // Outside a spot's frustum: no occlusion information, so unshadowed (the cone is dark there
    // anyway). A point's guard-banded face always contains the point it was selected for.
    if (kind == 1 && (ndc.z > 1.0 || abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0)) return 0.0;

    // Constant + slope-scaled depth bias in texels, the slope term widened by the kernel's reach
    // (see toyengine's toy_shadow_bias_texels), converted to this view's perspective depth.
    float nl  = clamp(ndl, 1e-3, 1.0);
    float tan_theta = min(sqrt(max(1.0 - nl * nl, 0.0)) / nl, GFX_LOCAL_SHADOWS.bias.z);
    float bias_texels = GFX_LOCAL_SHADOWS.bias.x + GFX_LOCAL_SHADOWS.bias.y * tan_theta * (1.0 + radius);
    float ref = ndc.z - gfx_local_shadow_depth_bias(sh, bias_texels * texel_world, clip.w);

    float atlas  = max(GFX_LOCAL_SHADOWS.info.y, 1.0);
    vec2  origin = gfx_local_shadow_tile_origin(sh, face);
    vec2  lo     = origin + vec2(0.5 / atlas);
    vec2  hi     = origin + vec2(sh.tile.z - 0.5 / atlas);
    vec2  uv     = origin + (ndc.xy * 0.5 + 0.5) * sh.tile.z;

    float occ;
    if (radius <= 0.0 || taps <= 1) {
        occ = texture(local_shadow_atlas, vec3(clamp(uv, lo, hi), ref));
    } else {
        const float GOLDEN_ANGLE = 2.39996323;
        float r_uv = radius / atlas;
        occ = 0.0;
        for (int i = 0; i < 32; ++i) {
            if (i >= taps) break;
            float r = sqrt((float(i) + 0.5) / float(taps));
            float a = float(i) * GOLDEN_ANGLE + rotation;
            vec2  o = r * r_uv * vec2(cos(a), sin(a));
            occ += texture(local_shadow_atlas, vec3(clamp(uv + o, lo, hi), ref));
        }
        occ /= float(taps);
    }
    return occ * sh.pcf.w;
}

/// One hardware-compare tap at `p` -- for volumetric in-scatter, where a shadow only modulates
/// a sample of fog and a kernel would be wasted. No normal offset (fog has no surface); a fixed
/// two-texel depth bias.
float gfx_local_shadow_tap(int slot, vec3 p) {
    GfxLocalShadow sh = GFX_LOCAL_SHADOWS.shadows[slot];
    int kind = int(sh.tile.w + 0.5);
    if (kind == 0) return 0.0;
    int  face = (kind == 2) ? gfx_local_shadow_face(p - sh.light.xyz) : 0;
    vec4 clip = GFX_LOCAL_SHADOWS.view_proj[int(sh.proj.w + 0.5) + face] * vec4(p, 1.0);
    if (clip.w <= 1e-4) return 0.0;
    vec3 ndc = clip.xyz / clip.w;
    if (kind == 1 && (ndc.z > 1.0 || abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0)) return 0.0;
    float texel_world = 2.0 * clip.w * sh.proj.y / max(sh.proj.z, 1.0);
    float ref    = ndc.z - gfx_local_shadow_depth_bias(sh, 2.0 * texel_world, clip.w);
    float atlas  = max(GFX_LOCAL_SHADOWS.info.y, 1.0);
    vec2  origin = gfx_local_shadow_tile_origin(sh, face);
    vec2  uv     = clamp(origin + (ndc.xy * 0.5 + 0.5) * sh.tile.z,
                         origin + vec2(0.5 / atlas), origin + vec2(sh.tile.z - 0.5 / atlas));
    return texture(local_shadow_atlas, vec3(uv, ref)) * sh.pcf.w;
}

#endif // GFX_LOCAL_SHADOW_GLSL
