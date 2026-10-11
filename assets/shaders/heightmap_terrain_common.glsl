#ifndef HEIGHTMAP_TERRAIN_COMMON_GLSL
#define HEIGHTMAP_TERRAIN_COMMON_GLSL

// heightmap_terrain_common.glsl -- reading a heightmap terrain's grid (toyengine/world/
// heightmap_terrain.h). Every stage of the `heightmap_terrain` shader builds the surface from
// here: the vertex / evaluation hooks place each vertex on the grid, the control stages size
// edges from the same placement, and the fragment stage takes per-pixel normals from it.
//
// The includer has declared, before this file:
//   u_displacement   sampler2D: the height grid, R32F metres (the material set's binding 4)
//   gfx_world        (gfx/surface/world.glsl): gfx_world.terrain[slot] is this terrain's numbers
//
// ### Placement
//
// A chunk is one instance of a shared flat patch (object xy in [0, 1]); its model matrix's
// translation is the chunk's corner. The chunk's integer index is recovered from that corner, and
// a vertex's grid coordinate is (index + patch xy) * texels-per-chunk -- so a vertex on a border
// two chunks share comes out bit-identical from both, and no crack opens between them. The model
// matrix's z (min / max height of the chunk) only serves culling; height comes from the grid.
//
// Heights are read with texelFetch and filtered by hand: a 32-bit float format is not filterable
// everywhere, and the hand filter is exactly the CPU's (HeightmapData::sample_height()).

int ht_slot(float p) { return clamp(int(p + 0.5), 0, 3); }

float ht_texel(int slot, ivec2 c) {
    ivec2 n = gfx_world.terrain[slot].dims.xy;
    return texelFetch(u_displacement, clamp(c, ivec2(0), n - 1), 0).r;
}

/// Bilinear height (m above the terrain's origin) at grid coordinate t (texel units).
float ht_height(int slot, vec2 t) {
    vec2 f0 = floor(t);
    ivec2 i = ivec2(f0);
    vec2 f = t - f0;
    float a = ht_texel(slot, i);
    float b = ht_texel(slot, i + ivec2(1, 0));
    float c = ht_texel(slot, i + ivec2(0, 1));
    float d = ht_texel(slot, i + ivec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

/// Unit surface normal at grid coordinate t (central differences one texel apart).
vec3 ht_normal(int slot, vec2 t) {
    vec2 cell = gfx_world.terrain[slot].cell.xy;
    float hl = ht_height(slot, t - vec2(1.0, 0.0));
    float hr = ht_height(slot, t + vec2(1.0, 0.0));
    float hd = ht_height(slot, t - vec2(0.0, 1.0));
    float hu = ht_height(slot, t + vec2(0.0, 1.0));
    return normalize(vec3((hl - hr) / (2.0 * cell.x), (hd - hu) / (2.0 * cell.y), 1.0));
}

/// Grid coordinate of a patch vertex (see Placement above).
vec2 ht_grid_coord(int slot, vec3 pos_os, mat4 model) {
    vec4 o = gfx_world.terrain[slot].origin;
    vec2 cell = gfx_world.terrain[slot].cell.xy;
    vec2 chunk = floor((model[3].xy - o.xy) / o.w + 0.5);
    // Snap the patch's own borders to exactly 0 / 1: interpolation (tessellation) can land a
    // border vertex an ulp off, and the neighbouring chunk computes the exact value.
    vec2 p = pos_os.xy;
    p = mix(p, vec2(0.0), lessThan(abs(p), vec2(1e-5)));
    p = mix(p, vec2(1.0), lessThan(abs(p - 1.0), vec2(1e-5)));
    vec2 t = (chunk + p) * (o.w / cell);
    return clamp(t, vec2(0.0), vec2(gfx_world.terrain[slot].dims.xy - 1));
}

/// Grid coordinate of a world position.
vec2 ht_grid_coord_ws(int slot, vec3 p) {
    return (p.xy - gfx_world.terrain[slot].origin.xy) / gfx_world.terrain[slot].cell.xy;
}

/// World position of grid coordinate t on the surface.
vec3 ht_world(int slot, vec2 t) {
    vec4 o = gfx_world.terrain[slot].origin;
    return vec3(o.xy + t * gfx_world.terrain[slot].cell.xy, o.z + ht_height(slot, t));
}

bool ht_deep_snow(int slot) { return (gfx_world.terrain[slot].dims.w & 1) != 0; }
bool ht_puddles(int slot) { return (gfx_world.terrain[slot].dims.w & 2) != 0; }

/// Deep snow lies only on ground flat enough to hold it.
float ht_snow_hold(vec3 n) { return smoothstep(0.6, 0.85, n.z); }

#endif // HEIGHTMAP_TERRAIN_COMMON_GLSL
