#ifndef GFX_SURFACE_SNOW_GLSL
#define GFX_SURFACE_SNOW_GLSL

// gfx/surface/snow.glsl -- the lying-snow cover layer every opaque surface inherits: the
// G-buffer fragment backbone (gfx/surface/gbuffer_fs.glsl) calls gfx_snow_apply() on the finished
// GfxSurface, after the shader's own hook. So stock, terrain, terrain_styled, triplanar and
// foliage all gather snow without knowing about it.
//
// Where it settles: on surfaces whose GEOMETRIC normal faces up (the mesh's, not the normal
// map's -- snow lies on a slope, not on the bumps of a brick), that are open to the sky (the
// weather's precipitation map, gfx_world_open_sky(): nothing under a roof or a tree), with a
// patchy noise edge that closes as the cover (gfx_world.snow.x, WeatherState::snow_cover)
// rises. Steeper faces only take snow at heavier cover.
//
// Opting out: a material with `snow: false` (PBRMaterial::snow -> surface_ext.z bit 0), or a
// shader defining GFX_SURFACE_NO_SNOW before including the backbone (editor_paint, the deep
// `snow` shader that draws snow itself).
//
// Two looks, the weather's `snow_patch_style` (gfx_world.snow_style.x): soft (the default) --
// value-noise drifts with a soft edge -- or hard -- round, crisp-edged patches that grow and merge
// (gfx/surface/snow_patches.glsl), a stylized / toon look.
//
// Needs world.glsl (the backbone includes it).

#include <gfx/surface/snow_patches.glsl>

const vec3 GFX_SNOW_ALBEDO = vec3(0.86, 0.89, 0.93);

float gfx_snow_hash_(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float gfx_snow_noise_(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = gfx_snow_hash_(i), b = gfx_snow_hash_(i + vec2(1, 0));
    float c = gfx_snow_hash_(i + vec2(0, 1)), d = gfx_snow_hash_(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
/// 0..1 patchiness: broad drifts plus finer breakup.
float gfx_snow_patches(vec2 xy) {
    return gfx_snow_noise_(xy * 0.35) * 0.6 + gfx_snow_noise_(xy * 1.7) * 0.3 + gfx_snow_noise_(xy * 7.0) * 0.1;
}

/**
 * How much snow lies at world point p with geometric (unbent, front-facing) normal geo_n: 0..1.
 * `cover` is the weather's lying snow.
 */
float gfx_snow_amount(vec3 p, vec3 geo_n, float cover) {
    if (cover <= 0.0) return 0.0;
    if (gfx_world.snow_style.x > 0.5) {
        // Hard patches. The field and its screen derivative are taken before any per-pixel
        // branch, so fwidth() sees all four quad lanes.
        float field = gfx_snow_blobs(p.xy, gfx_world.snow_style.y);
        float w = max(fwidth(field), 1e-4);
        float up = smoothstep(mix(0.85, 0.45, cover), mix(0.95, 0.65, cover), geo_n.z);
        float open = gfx_world_open_sky(p + vec3(0.0, 0.0, 0.35 + 0.35 * gfx_world.occl.z), 0.5);
        float receptive = up * open;
        if (receptive <= 0.0) return 0.0;
        float v = field - (1.05 - 1.15 * cover) - (1.0 - receptive) * 1.5;   // gfx_snow_patch_value()
        return clamp(v / w + 0.5, 0.0, 1.0);
    }
    // Up-facing: flat ground takes it first; steeper faces as the cover deepens.
    float up = smoothstep(mix(0.85, 0.45, cover), mix(0.95, 0.65, cover), geo_n.z);
    if (up <= 0.0) return 0.0;
    // Open to the sky -- a little tolerance for the 1 m map cells on slopes.
    float open = gfx_world_open_sky(p + vec3(0.0, 0.0, 0.35 + 0.35 * gfx_world.occl.z), 0.5);
    float receptive = up * open;
    if (receptive <= 0.0) return 0.0;
    // Patches: the noise threshold drops with cover, so drifts appear, then join up.
    float n = gfx_snow_patches(p.xy);
    float edge = 1.0 - cover * 1.25;
    float patches = smoothstep(edge - 0.06, edge + 0.06, n * mix(0.85, 1.0, receptive));
    return patches * receptive;
}

/// Lays the snow on a finished surface. geo_n: the geometric world normal (front-facing).
#define gfx_snow_apply(s, geo_n)                                                                  \
    {                                                                                              \
        float gfx_snow_a_ = gfx_snow_amount((s).position_ws, (geo_n), gfx_world.snow.x);           \
        if (gfx_snow_a_ > 0.0) {                                                                   \
            float gfx_snow_sparkle_ = gfx_snow_hash_(floor((s).position_ws.xy * 40.0)) * 0.06;       \
            (s).albedo    = mix((s).albedo, GFX_SNOW_ALBEDO + gfx_snow_sparkle_, gfx_snow_a_);      \
            (s).roughness = mix((s).roughness, 0.8, gfx_snow_a_);                                  \
            (s).metallic  = mix((s).metallic, 0.0, gfx_snow_a_);                                   \
            (s).ao        = mix((s).ao, 1.0, gfx_snow_a_ * 0.5);                                   \
            (s).emissive  = mix((s).emissive, vec3(0.0), gfx_snow_a_);                             \
            (s).normal_ws = normalize(mix((s).normal_ws, (geo_n), gfx_snow_a_ * 0.85));             \
        }                                                                                          \
    }

#endif // GFX_SURFACE_SNOW_GLSL
