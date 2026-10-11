#ifndef GFX_SURFACE_WETNESS_GLSL
#define GFX_SURFACE_WETNESS_GLSL

// gfx/surface/wetness.glsl -- rain on a surface: the weather's wetness (gfx_world.snow.z, 0..1,
// rising while it rains and drying after) darkens porous ground and makes it glossy where it is
// open to the sky, and fills puddles where the caller says water gathers. Fragment stage, after a
// surface has its look (needs GfxSurface from gbuffer_fs.glsl and world.glsl).
//
//   puddle   0..1: how much of a hollow this point is -- the caller's measure (the heightmap
//            terrain uses its grid's cavity and flatness). Puddles fill as the wetness rises:
//            the deepest parts first.

/// Cheap value noise to break puddle edges up (world xy, metres).
float gfx_wet_noise_(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void gfx_wet_apply(inout GfxSurface s, vec3 geo_n, float puddle) {
    float wet = clamp(gfx_world.snow.z, 0.0, 1.0);
    if (wet <= 0.0) return;
    float open = gfx_world_open_sky(s.position_ws + geo_n * 0.05 + vec3(0.0, 0.0, 0.3), 0.5);
    float w = wet * open;
    if (w <= 0.0) return;

    // Soaked ground: darker (water fills the pores; metals don't soak), glossier.
    float porous = 1.0 - s.metallic;
    s.albedo   *= mix(1.0, 0.55, w * porous);
    s.roughness = mix(s.roughness, max(0.08, s.roughness * 0.35), w);

    // Puddles: the hollowest parts fill first, edges broken by noise.
    float n = gfx_wet_noise_(s.position_ws.xy * 0.6) * 0.35 + gfx_wet_noise_(s.position_ws.xy * 2.3) * 0.15;
    float fill = smoothstep(1.0 - w, 1.0 - w + 0.25, puddle - n * 0.5 + 0.25 * w);
    if (fill > 0.0) {
        s.albedo    *= mix(1.0, 0.7, fill);
        s.roughness  = mix(s.roughness, 0.03, fill);
        s.metallic   = mix(s.metallic, 0.0, fill);
        s.normal_ws  = normalize(mix(s.normal_ws, vec3(0.0, 0.0, 1.0), fill));
    }
}

#endif // GFX_SURFACE_WETNESS_GLSL
