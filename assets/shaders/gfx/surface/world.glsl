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

layout(set = GFX_WORLD_SET, binding = 0) uniform SurfaceWorldUBO {
    vec4  tess_view;      // xyz main camera position, w pixel scale (0.5 * render height * proj[1][1])
    vec4  snow;           // x cover 0..1, y deep-snow depth (m), z wetness, w time (s)
    vec4  wind;           // xyz wind (m/s), w trench scale (m at unorm 1)
    vec4  occl;           // xy origin, z cell, w 1 = map valid
    ivec4 occl_dims;      // x nx, y ny
    vec4  occl_fallback;  // x fallback height
    vec4  field;          // z cell, w 1 = trench field valid
    ivec4 field_dims;     // x n, yz window min cell index
    vec4  snow_style;     // x 1 = hard-edged patches, y patch size (m)
} gfx_world;

layout(std430, set = GFX_WORLD_SET, binding = 1) readonly buffer GfxOcclusionHeights { float gfx_occl_heights[]; };
layout(std430, set = GFX_WORLD_SET, binding = 2) readonly buffer GfxSnowTrench { uint gfx_snow_trench[]; };

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
