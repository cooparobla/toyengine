#ifndef GFX_SURFACE_WORLD_GLSL
#define GFX_SURFACE_WORLD_GLSL

// gfx/surface/world.glsl -- the "surface world" set (toyengine/render/surface_world.h): world-space
// data every opaque surface stage may read -- the tessellation view, snow / wetness, the
// weather's precipitation occlusion map and the snow trench field.
//
// The including backbone #defines GFX_WORLD_SET to its set index first (the G-buffer binds it
// at 2, the shadow passes at 1). The block below must match SurfaceWorldUBO byte for byte.

#ifndef GFX_WORLD_SET
#define GFX_WORLD_SET 2
#endif

// One heightmap terrain (toyengine/render/surface_world.h TerrainGpuParams; see
// heightmap_terrain_common.glsl for how the terrain shader reads it).
struct GfxTerrain {
    vec4  origin;        // xyz grid corner at height 0, w chunk size (m)
    vec4  cell;          // xy metres per height texel, zw terrain size (m)
    ivec4 dims;          // x nx, y ny, z layer count, w flags (1 deep snow, 2 puddles)
    vec4  misc;          // x height (m), y splat texels per side
    vec4  layer[8];      // x 1/tiling, y roughness, z metallic, w normal strength
    vec4  layer_tri[8];  // x triplanar, y sharpness, z has normal map
    vec4  tint[8];       // rgb albedo multiplier
};

// One local environmental influence (toyengine/render/surface_world.h EnvSourceGpu, from
// toyengine/env/env_source.h). params.z is a flag word: 1 disc, 2 radial, 4 max, 8 override.
struct GfxEnvSource {
    vec4 pos_radius;  // xyz world centre, w radius (m); w <= 0 = unused
    vec4 wind;        // directional: xyz m/s. radial: x outward, y vertical. w turbulence
    vec4 params;      // x falloff, y disc half-height (0 = unbounded), z flags, w wetness
};

layout(set = GFX_WORLD_SET, binding = 0) uniform SurfaceWorldUBO {
    vec4  tess_view;      // xyz main camera position, w pixel scale (0.5 * render height * proj[1][1])
    vec4  snow;           // x cover 0..1, y deep-snow depth (m), z wetness, w time (s)
    vec4  wind;           // xyz mean wind (m/s), w trench scale (m at unorm 1)
    vec4  occl;           // xy origin, z cell, w 1 = map valid
    ivec4 occl_dims;      // x nx, y ny
    vec4  occl_fallback;  // x fallback height
    vec4  field;          // z cell, w 1 = trench field valid
    ivec4 field_dims;     // x n, yz window min cell index
    vec4  snow_style;     // x 1 = hard-edged patches, y patch size (m)
    vec4  wind_gust;      // x gust strength, y gust clock (s) -- gfx_world_wind_at()
    ivec4 terrain_count;  // x heightmap terrains in use, y local env sources in use
    GfxTerrain terrain[4];
    // Appended AFTER terrain[] to match SurfaceWorldUBO: inserting ahead of it would shift
    // every terrain offset.
    GfxEnvSource env[8];
} gfx_world;

layout(std430, set = GFX_WORLD_SET, binding = 1) readonly buffer GfxOcclusionHeights { float gfx_occl_heights[]; };
layout(std430, set = GFX_WORLD_SET, binding = 2) readonly buffer GfxSnowTrench { uint gfx_snow_trench[]; };

// ---------------------------------------------------------------------------------------
// Local environmental influences -- the SAME fold as toyengine/env/env_field.h's
// local_wind_at() / wind_at() / wetness_at(). Keep the two in step, exactly as this file and
// toyengine/weather/wind.h already are for the global wind.
//
// Every one of these early-outs on `gfx_world.terrain_count.y == 0`, so a scene with no local
// sources -- which is every scene that does not author one -- pays nothing. That matters:
// these run per vertex in the foliage G-buffer stages AND in their shadow and shadow-cube
// twins, over tens of thousands of grass instances.
// ---------------------------------------------------------------------------------------

/// How strongly source `i` reaches p, 0..1. Mirrors env_source.h env_falloff().
float gfx_env_falloff(int i, vec3 p) {
    vec4 pr = gfx_world.env[i].pos_radius;
    float radius = pr.w;
    if (radius <= 0.0) return 0.0;
    vec3 d = p - pr.xyz;
    int flags = int(gfx_world.env[i].params.z);
    float r;
    if ((flags & 1) != 0) {              // disc: a cylinder about +Z, optionally capped
        float height = gfx_world.env[i].params.y;
        if (height > 0.0 && abs(d.z) > height) return 0.0;
        r = length(d.xy);
    } else {
        r = length(d);
    }
    if (r >= radius) return 0.0;
    float inner = radius * (1.0 - clamp(gfx_world.env[i].params.x, 0.0, 1.0));
    if (r <= inner) return 1.0;
    float t = (radius - r) / max(radius - inner, 1e-6);
    return t * t * (3.0 - 2.0 * t);
}

/// Source `i`'s wind at p at full strength, no falloff. Mirrors env_wind_full().
vec3 gfx_env_wind_full(int i, vec3 p) {
    vec4 w = gfx_world.env[i].wind;
    int flags = int(gfx_world.env[i].params.z);
    if ((flags & 2) == 0) return w.xyz;            // directional
    vec2 radial = p.xy - gfx_world.env[i].pos_radius.xy;
    float len = length(radial);
    vec2 dir = len > 1e-4 ? radial / len : vec2(0.0);
    return vec3(dir * w.x, w.y);                   // x outward, y vertical
}

/// The local sources' wind (m/s) at p; no weather, no gusts. Add sources sum, and of the Max
/// sources only the strongest contributes. Override is applied by gfx_world_wind_at().
vec3 gfx_env_local_wind_at(vec3 p) {
    int n = gfx_world.terrain_count.y;
    if (n <= 0) return vec3(0.0);
    vec3 added = vec3(0.0), strongest = vec3(0.0);
    float strongest_len2 = 0.0;
    for (int i = 0; i < n; ++i) {
        int flags = int(gfx_world.env[i].params.z);
        if ((flags & 8) != 0) continue;            // override: not part of the combine
        float w = gfx_env_falloff(i, p);
        if (w <= 0.0) continue;
        vec3 v = gfx_env_wind_full(i, p) * w;
        if ((flags & 4) != 0) {
            float len2 = dot(v, v);
            if (len2 > strongest_len2) { strongest_len2 = len2; strongest = v; }
        } else {
            added += v;
        }
    }
    return added + strongest;
}

/// Extra flutter at p, 0..1 -- local sources only (the weather's gusts are in the wind).
float gfx_env_turbulence_at(vec3 p) {
    int n = gfx_world.terrain_count.y;
    if (n <= 0) return 0.0;
    float added = 0.0, strongest = 0.0, over = 0.0;
    float over_w = 0.0;
    for (int i = 0; i < n; ++i) {
        float w = gfx_env_falloff(i, p);
        if (w <= 0.0) continue;
        int flags = int(gfx_world.env[i].params.z);
        float v = gfx_world.env[i].wind.w;
        if ((flags & 8) != 0) {
            if (w > over_w) { over_w = w; over = v; }
        } else if ((flags & 4) != 0) {
            if (abs(v * w) > abs(strongest)) strongest = v * w;
        } else {
            added += v * w;
        }
    }
    float combined = added + strongest;
    return clamp(mix(combined, over, over_w), 0.0, 1.0);
}

/// Surface wetness at p, 0..1: the weather's (gfx_world.snow.z) plus every local source's.
float gfx_env_wetness_at(vec3 p) {
    float base = gfx_world.snow.z;
    int n = gfx_world.terrain_count.y;
    if (n <= 0) return clamp(base, 0.0, 1.0);
    float added = 0.0, strongest = 0.0, over = 0.0;
    float over_w = 0.0;
    for (int i = 0; i < n; ++i) {
        float w = gfx_env_falloff(i, p);
        if (w <= 0.0) continue;
        int flags = int(gfx_world.env[i].params.z);
        float v = gfx_world.env[i].params.w;
        if ((flags & 8) != 0) {
            if (w > over_w) { over_w = w; over = v; }
        } else if ((flags & 4) != 0) {
            if (abs(v * w) > abs(strongest)) strongest = v * w;
        } else {
            added += v * w;
        }
    }
    float combined = base + added + strongest;
    return clamp(mix(combined, over, over_w), 0.0, 1.0);
}

/// The weather's wind (m/s, horizontal) at world point p: the mean wind plus gust fronts
/// rolling downwind. The SAME formula as toyengine/weather/wind.h wind_at() -- keep the two in
/// step. No local influences; gfx_world_wind_at() is the full answer.
vec3 gfx_world_global_wind_at(vec3 p) {
    vec3 wind = gfx_world.wind.xyz;
    float speed = length(wind.xy);
    float gust = gfx_world.wind_gust.x;
    if (speed < 1e-4 || gust <= 0.0) return wind;
    float t = gfx_world.wind_gust.y;
    vec2 dir = wind.xy / speed;
    float along = dot(p.xy, dir);
    float cross_ = dot(p.xy, vec2(-dir.y, dir.x));
    float a = along - speed * t;
    float g = 0.45 * sin(0.05 * a + 1.5 * sin(0.021 * cross_))
            + 0.25 * sin(0.13 * a + 0.09 * cross_ + 2.1)
            + 0.30 * (0.6 * sin(0.7 * t) + 0.4 * sin(1.9 * t + 1.3));
    return wind * max(0.0, 1.0 + gust * g);
}

/// The wind (m/s) at world point p: the weather's mean plus gust fronts rolling downwind, plus
/// every local influence. The global term is the SAME formula as toyengine/weather/wind.h
/// wind_at(), and the local fold the same as toyengine/env/env_field.h wind_at() -- keep all
/// three in step.
vec3 gfx_world_wind_at(vec3 p) {
    vec3 global = gfx_world_global_wind_at(p);
    int n = gfx_world.terrain_count.y;
    if (n <= 0) return global;
    vec3 base = global + gfx_env_local_wind_at(p);
    // Override last: of several overlapping ones the one reaching p most strongly wins, so the
    // answer never depends on the order they were packed in.
    int best = -1;
    float over_w = 0.0;
    for (int i = 0; i < n; ++i) {
        if ((int(gfx_world.env[i].params.z) & 8) == 0) continue;
        float w = gfx_env_falloff(i, p);
        if (w > over_w) { over_w = w; best = i; }
    }
    if (best < 0) return base;
    return mix(base, gfx_env_wind_full(best, p), over_w);
}

/// The precipitation map's surface height at world xy (bilinear; outside the map: the fallback
/// plane). A point is OPEN to the sky (rain / snow reach it) when it lies at or above this.
float gfx_world_occlusion_height(vec2 xy) {
    if (gfx_world.occl.w < 0.5) return -1e30;
    ivec2 n = gfx_world.occl_dims.xy;
    vec2 g = (xy - gfx_world.occl.xy) / gfx_world.occl.z - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    float h[4];
    for (int k = 0; k < 4; ++k) {
        ivec2 c = i0 + ivec2(k & 1, k >> 1);
        bool inside = c.x >= 0 && c.y >= 0 && c.x < n.x && c.y < n.y;
        h[k] = inside ? gfx_occl_heights[c.y * n.x + c.x] : gfx_world.occl_fallback.x;
    }
    // MAX of the nearest cell and the bilinear: a roof edge must keep the roof's height on the
    // roof side rather than ramping down across the eave (which would open the wall top).
    float bl = mix(mix(h[0], h[1], f.x), mix(h[2], h[3], f.x), f.y);
    ivec2 c = clamp(ivec2(floor(g + 0.5)), ivec2(0), n - 1);
    float near = (g.x < -0.5 || g.y < -0.5 || g.x > float(n.x) - 0.5 || g.y > float(n.y) - 0.5)
               ? gfx_world.occl_fallback.x : gfx_occl_heights[c.y * n.x + c.x];
    return max(bl, near);
}

/// 0..1: how open to the sky world point p is (1 = nothing above it). `soft` metres of fade.
float gfx_world_open_sky(vec3 p, float soft) {
    if (gfx_world.occl.w < 0.5) return 1.0;
    float h = gfx_world_occlusion_height(p.xy);
    return smoothstep(h - soft, h - soft * 0.25, p.z);
}

/// The snow trench depth (m) pressed in at world xy -- the SnowField, toroidal in world cells.
float gfx_world_trench_cell_(ivec2 c) {
    int n = gfx_world.field_dims.x;
    ivec2 lo = gfx_world.field_dims.yz;
    if (c.x < lo.x || c.y < lo.y || c.x >= lo.x + n || c.y >= lo.y + n) return 0.0;
    ivec2 t = ((c % n) + n) % n;
    int idx = t.y * n + t.x;
    uint word = gfx_snow_trench[idx >> 1];
    uint v = (idx & 1) == 0 ? (word & 0xFFFFu) : (word >> 16);
    return float(v) / 65535.0 * gfx_world.wind.w;
}
float gfx_world_trench(vec2 xy) {
    if (gfx_world.field.w < 0.5) return 0.0;
    vec2 g = xy / gfx_world.field.z - 0.5;
    ivec2 i0 = ivec2(floor(g));
    vec2 f = g - vec2(i0);
    float a = gfx_world_trench_cell_(i0);
    float b = gfx_world_trench_cell_(i0 + ivec2(1, 0));
    float c = gfx_world_trench_cell_(i0 + ivec2(0, 1));
    float d = gfx_world_trench_cell_(i0 + ivec2(1, 1));
    // Fades out over the outer fifth of the window, so a track leaving it (the window follows
    // the view) eases away instead of vanishing. As toy::world::SnowField::edge_fade().
    float half_n = 0.5 * float(gfx_world.field_dims.x);
    vec2  rel    = abs(g + 0.5 - (vec2(gfx_world.field_dims.yz) + half_n)) / half_n;
    float fade   = 1.0 - smoothstep(0.8, 1.0, max(rel.x, rel.y));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y) * fade;
}

#endif // GFX_SURFACE_WORLD_GLSL
