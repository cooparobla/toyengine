
// `terrain_styled` surface shader: the G-buffer fragment stage for styled terrain chunks
// (toyengine/world/terrain_chunk.h's mesh_chunk_styled, toyengine/world/README.md).
//
// Which surface kind a pixel shows is decided HERE, per pixel, not per triangle. Every vertex
// carries a packed blend code in uv.x (toy::world::encode_surface_blend(), tile_topology.h): the
// kinds the surface may show and how to choose between them. Boundaries stay straight where the
// mesh puts them; what this rounds off are their CORNERS, so two kinds meet in smooth lines
// rather than the tile grid's right angles:
//
//   * Top    -- a quadrant of a tile top knows its level neighbours' kinds. Where both of its
//               edge neighbours differ from it, the tile has a convex corner there: pixels
//               outside a quarter circle take the neighbours' kind. The other kind's inner
//               corner is the same curve seen from the other side, so it rounds too.
//   * Lip    -- a cliff's rounded top edge shows turf where the surface faces up past a
//               threshold, so the grass line follows the round-over's own curve.
//   * Flat   -- one kind.
//
// The atlas supplies each kind's COLOUR and ROUGHNESS, sampled once at its cell centre; the
// detail is procedural, in world space:
//
//   * Value noise at a few frequencies, evaluated on the world position -- so it runs seamlessly
//     across quadrants, wall halves, rounded lips and chunk borders, which projected UVs cannot,
//     and never repeats per tile.
//   * Each octave fades out as it approaches the pixel's footprint (screen-space derivatives of
//     the world position). That is a mip chain computed analytically: crisp up close, calm at a
//     distance, no shimmer -- and it needs no mipmapped textures, which this engine's texture
//     path does not produce.
//   * The same noise, through its analytic gradient, perturbs the shading normal slightly, so a
//     surface catches light like a material rather than a print.
//
// Per-kind character (blotchiness, grain, specks, strata, bump) is the table below, indexed by
// toy::world::TileKind -- the atlas's own order, appended, never reordered.
//
// shader_params (filled in by TerrainSystem): x = atlas cell columns, y = atlas cell rows,
// z = height_step, w = tile_size (world units).
//
// This is the body of terrain_styled.frag; terrain_styled_planet.frag builds it with
// TERRAIN_PLANET for a planet's chunks, which are bent onto a sphere: uv.y then carries each
// vertex's "how far up it faces" (away from the planet), and wall strata have no one vertical to
// band along. Without the define none of that code exists.

#define GFX_SURFACE_FRAGMENT
// Term-level kill switches for bisecting a rendering artifact against individual detail terms
// (TERRAIN_STYLED_FLAT above the hook kills them all). None are defined in a normal build.

// The baked detail texture (world/terrain_detail_texture.h) is bound in the material set's
// NORMAL slot: styled terrain authors no normal map, and that slot is the one free sampler in
// the existing material layout -- taking it costs no descriptor-layout change, which would
// renumber every set that follows (see DeferredLightingPass's set-index note). The backbone's
// own use of that slot is neutralised by returning a flat tangent-space normal from the
// SAMPLE_NORMAL hook, so the surface keeps its geometric normal until this shader bends it.
#define u_terrain_detail u_normal_map
#define GFX_SURFACE_SAMPLE_NORMAL(tex, uv) vec4(0.5, 0.5, 1.0, 1.0)

/// World size of one tile of the baked detail texture, and its edge length in texels. Must
/// match bake_terrain_detail_texture()'s `world_period` / `resolution` defaults.
const float k_detail_period = 16.0;
const float k_detail_texels = 512.0;
vec4 styled_sample(sampler2D tex, vec2 uv);
#define GFX_SURFACE_SAMPLE(tex, uv) styled_sample(tex, uv)
#include <gfx/surface/gbuffer_fs.glsl>
#include <gfx/surface/wetness.glsl>

// Twelve, though TileKind has THIRTEEN (cubeworld appended Road). The clamp below is doing real
// work: planet chunks decode surface nibbles above the table's range, and raising this to 13
// moved 1.2% of planet_demo's pixels -- 27x the capture noise -- by routing them to a new entry
// instead of salt. Whatever those nibbles are, they are not roads: the engine's own bake never
// produces TileKind::Road. Extending this table means fixing that decode first.
const int k_kind_count = 12;

// x = broad blotches, y = mid grain, z = fine grain, w = specks (amplitudes, as albedo fractions)
const vec4 k_detail[k_kind_count] = vec4[](
    vec4(0.10, 0.08, 0.06, 0.10),  // grass
    vec4(0.08, 0.09, 0.07, 0.12),  // dirt
    vec4(0.06, 0.08, 0.05, 0.05),  // stone
    vec4(0.04, 0.04, 0.06, 0.05),  // sand
    vec4(0.02, 0.02, 0.03, 0.02),  // snow
    vec4(0.08, 0.09, 0.06, 0.06),  // rock
    vec4(0.05, 0.03, 0.00, 0.00),  // water
    vec4(0.03, 0.03, 0.02, 0.00),  // ice
    vec4(0.10, 0.08, 0.06, 0.10),  // moss
    vec4(0.07, 0.08, 0.06, 0.10),  // clay
    vec4(0.07, 0.08, 0.06, 0.08),  // ash
    vec4(0.03, 0.03, 0.04, 0.03)); // salt

// x = strata (horizontal banding on walls), y = bump strength, z = warm/cool hue drift,
// w = speck polarity (+1 lighter specks, -1 darker)
const vec4 k_extra[k_kind_count] = vec4[](
    vec4(0.00, 0.30, 0.6,  1.0),   // grass: lighter blade flecks
    vec4(0.03, 0.45, 0.3,  1.0),   // dirt: pale pebbles
    vec4(0.10, 0.50, 0.1, -1.0),   // stone: strata, dark pits
    vec4(0.00, 0.20, 0.2,  1.0),   // sand
    vec4(0.00, 0.10, 0.0,  1.0),   // snow
    vec4(0.12, 0.55, 0.1, -1.0),   // rock
    vec4(0.00, 0.00, 0.0,  1.0),   // water
    vec4(0.00, 0.05, 0.0,  1.0),   // ice
    vec4(0.00, 0.30, 0.5,  1.0),   // moss
    vec4(0.04, 0.40, 0.3,  1.0),   // clay
    vec4(0.03, 0.40, 0.1, -1.0),   // ash
    vec4(0.00, 0.10, 0.0,  1.0));  // salt

// How readily a surface holds standing water, 0..1 -- what rain pools on and what it drains
// through. Sand and ash take it away; stone, clay and trodden ground keep it.
const float k_holds_water[k_kind_count] = float[](
    0.75,  // grass
    0.85,  // dirt
    1.00,  // stone
    0.10,  // sand
    0.00,  // snow -- the snow layer is what shows there, not a puddle
    0.90,  // rock
    0.00,  // water
    0.00,  // ice
    0.80,  // moss
    1.00,  // clay
    0.25,  // ash
    0.30); // salt

// Octave frequencies, cycles per world unit (one tile is one unit in the stock scenes).
const float k_freq_broad = 0.22;
const float k_freq_mid   = 1.7;
const float k_freq_fine  = 6.5;
const float k_freq_speck = 5.0;

// Radius, in tiles, of the curve that replaces a kind boundary's corner. At most 0.5, the
// quadrant's own size.
const float k_corner_radius = 0.4;
// Lip: turf where the surface normal's z exceeds this.
const float k_lip_threshold = 0.55;

int g_kind = -1;

int styled_kind();

/// The colour/roughness of this pixel's kind: one tap at the centre of its atlas cell.
vec4 styled_sample(sampler2D tex, vec2 uv) {
    vec2 grid = max(gfx_params.xy, vec2(1.0));
    int kind = styled_kind();
    vec2 cell = vec2(float(kind % int(grid.x)), float(kind / int(grid.x)));
    return textureLod(tex, (cell + 0.5) / grid, 0.0);
}

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

/// 3D value noise in [-1, 1] with its analytic gradient (quintic fade, so the gradient is
/// continuous and the bump has no creases at lattice planes).
vec4 noise_d(vec3 x) {
    vec3 i = floor(x);
    vec3 f = x - i;
    vec3 u  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);

    float a = hash13(i + vec3(0, 0, 0)), b = hash13(i + vec3(1, 0, 0));
    float c = hash13(i + vec3(0, 1, 0)), d = hash13(i + vec3(1, 1, 0));
    float e = hash13(i + vec3(0, 0, 1)), g = hash13(i + vec3(1, 0, 1));
    float h = hash13(i + vec3(0, 1, 1)), k = hash13(i + vec3(1, 1, 1));

    float k0 = a, k1 = b - a, k2 = c - a, k3 = e - a;
    float k4 = a - b - c + d, k5 = a - c - e + h, k6 = a - b - e + g, k7 = -a + b + c - d + e - g - h + k;

    float v = k0 + k1 * u.x + k2 * u.y + k3 * u.z + k4 * u.x * u.y + k5 * u.y * u.z + k6 * u.z * u.x
            + k7 * u.x * u.y * u.z;
    vec3 grad = du * vec3(k1 + k4 * u.y + k6 * u.z + k7 * u.y * u.z,
                          k2 + k5 * u.z + k4 * u.x + k7 * u.z * u.x,
                          k3 + k6 * u.x + k5 * u.y + k7 * u.x * u.y);
    return vec4(2.0 * v - 1.0, 2.0 * grad);
}

/// How much of an octave of frequency `freq` survives a pixel footprint of `footprint` world
/// units: all of it while the octave is comfortably resolved, none of it once a wavelength
/// nears ~8 screen pixels.
///
/// The band (0.08, 0.15 cycles per pixel, i.e. wavelengths ~12 px down to ~6.7 px) sits far
/// below Nyquist on purpose, and the margin is temporal, not spatial: a still frame can
/// represent a 3-pixel wavelength, but under camera motion every sub-pixel shift re-samples it
/// to a visibly different pattern -- high-contrast grain at a handful of pixels per cycle reads
/// as per-pixel flicker that stops the instant the camera does. Bisected on cubeworld
/// (greenhollow, per-term kill switches): the fine octave carried at partial amplitude in the
/// 4-7 px/wavelength range was the whole in-motion surface flicker. This band keeps every
/// octave at full strength while it spans 12+ pixels and removes it before it can strobe; the
/// half-amplitude point sits near 10 px, where a sub-pixel shift moves a pixel by under one
/// level. Both edges are a look-vs-shimmer dial: raising them keeps grain visible further out
/// at the cost of motion shimmer returning first in the 7-10 px range.
float octave_weight(float freq, float footprint) {
    return 1.0 - smoothstep(0.08, 0.15, freq * footprint);
}

/// The kind this pixel shows -- see the file doc. Decided once per pixel and cached: the
/// backbone samples the atlas several times before calling the fragment hook.
int styled_kind() {
    if (g_kind >= 0) return g_kind;
    uint code = uint(max(frag_uv.x, 0.0) + 0.5);
    int k0 = int(code & 15u), k1 = int((code >> 4) & 15u), k2 = int((code >> 8) & 15u), k3 = int((code >> 12) & 15u);
    uint type = (code >> 16) & 3u;
    int kind = k0;

    // A checkerboard corner -- this kind again on the diagonal, the other on both edges -- would
    // round all four tiles into a pinwheel. Only the higher-indexed kind rounds there, so the
    // other connects diagonally through a smooth waist.
    bool checker = k3 == k0 && k1 == k2;
    if (type == 1u && k1 != k0 && k2 != k0 && (!checker || k0 > k1)) {
        // Top quadrant at a convex corner of its own kind. `e` is the pixel's distance, in
        // tiles, from the quadrant's two outer edges; the corner sits at e = (0, 0). The tile is
        // found from a point a quarter tile back toward its centre, which is robust right on an
        // edge.
        float tile = max(gfx_params.w, 1e-3);
        vec2  s = vec2(((code >> 18) & 1u) != 0u ? 1.0 : -1.0, ((code >> 19) & 1u) != 0u ? 1.0 : -1.0);
        vec2  p = frag_world_pos.xy;
        vec2  centre = (floor((p - s * 0.25 * tile) / tile) + 0.5) * tile;
        vec2  e = 0.5 - s * (p - centre) / tile;
        if (e.x < k_corner_radius && e.y < k_corner_radius &&
            length(vec2(k_corner_radius) - e) > k_corner_radius) {
            // Two different neighbours: the one that also holds the diagonal owns the corner, so
            // two rounded tiles bend into the same third kind (a smooth T, no sliver between
            // them); with no such neighbour, they split it along its diagonal.
            if (k1 == k2 || k3 == k1)  kind = k1;
            else if (k3 == k2)         kind = k2;
            else                       kind = e.x < e.y ? k1 : k2;
        }
    } else if (type == 2u) {
#ifdef TERRAIN_PLANET
        kind = frag_uv.y > k_lip_threshold ? k0 : k1;   // up = away from the planet (the warp's uv.y)
#else
        kind = normalize(frag_world_normal).z > k_lip_threshold ? k0 : k1;
#endif
    }
    g_kind = clamp(kind, 0, k_kind_count - 1);
    return g_kind;
}

void gfx_surface_fragment(inout GfxSurface s) {
    // TERRAIN_STYLED_FLAT: diagnostic kill switch -- returns before any detail term, leaving
    // the bare per-kind atlas colour. Bisecting a surface artifact starts here: if it survives
    // a flat albedo it is not this hook's doing.
#ifdef TERRAIN_STYLED_FLAT
    if (true) return;
#endif
    int kind = styled_kind();
    vec4 detail = k_detail[kind];
    vec4 extra  = k_extra[kind];

    vec3 p = s.position_ws;

    // The detail comes from a baked, tiling, MIPMAPPED texture (world/terrain_detail_texture.h),
    // sampled in world space by the surface's dominant axis -- not from noise evaluated here.
    //
    // Analytic noise cannot be prefiltered: an `octave_weight`-style fade decides how much of an
    // octave to keep from the pixel footprint, but still evaluates it at ONE point, so a
    // sub-pixel drift of that point changes the value. Over a voxel world that is per-pixel
    // flicker during any camera motion, settling only at a standstill -- bisected on cubeworld
    // to the fine octave, and tunable only into a flicker/washout trade. A mip chain is the real
    // thing: each level is the AVERAGE of the one above, and trilinear + anisotropic sampling
    // blends between levels continuously, per pixel, including on oblique faces a single
    // footprint scalar cannot describe. Same mechanism every engine uses for surface detail.
    //
    // Triplanar-lite: the two axes most in the surface's plane. A voxel world's faces are axis
    // aligned, so the dominant-normal pick is exact for them and merely approximate on the
    // rounded sets' bevels, where the detail is incidental anyway.
    vec3 an = abs(s.normal_ws);
    vec2 detail_uv = (an.z >= an.x && an.z >= an.y) ? p.xy
                   : (an.x >= an.y)                 ? p.yz
                                                    : p.xz;
    vec4 d = texture(u_terrain_detail, detail_uv / k_detail_period);
    // R/G/A are signed offsets around 0.5; B is already-thresholded speck coverage, baked so the
    // mip chain averages flecks into coverage rather than leaving a hard edge to alias.
    float blotch = d.r * 2.0 - 1.0;
    float grain  = d.g * 2.0 - 1.0;
    float speck  = d.b;
    float strata = d.a * 2.0 - 1.0;

    float shade = 0.0;
    // detail.x and .y were separate octaves of the same slow variation; the bake pairs them in
    // one channel, so their amplitudes add here.
    shade += (detail.x + detail.y) * blotch;
    shade += detail.z * grain;
    shade += detail.w * speck * extra.w;
    if (extra.x > 0.0) shade += extra.x * strata;
    float warm = blotch;

    s.albedo *= clamp(1.0 + shade, 0.5, 1.5);
    s.albedo += extra.z * 0.04 * warm * vec3(1.0, 0.35, -0.6);
    s.albedo = max(s.albedo, vec3(0.0));

    // Bump from the sampled detail's own slope, read with textureGrad offsets along the two
    // in-plane axes: the texture is prefiltered, so this slope fades with distance exactly as
    // the detail it describes does -- an analytic gradient would keep bending normals after the
    // features it belongs to were averaged away, which is specular aliasing by another route.
    vec2 base_uv = detail_uv / k_detail_period;
    float step_uv = 1.0 / k_detail_texels;
    float du = texture(u_terrain_detail, base_uv + vec2(step_uv, 0.0)).r
             - texture(u_terrain_detail, base_uv - vec2(step_uv, 0.0)).r;
    float dv = texture(u_terrain_detail, base_uv + vec2(0.0, step_uv)).r
             - texture(u_terrain_detail, base_uv - vec2(0.0, step_uv)).r;
    vec3 axis_u = (an.z >= an.x && an.z >= an.y) ? vec3(1,0,0) : (an.x >= an.y) ? vec3(0,1,0) : vec3(1,0,0);
    vec3 axis_v = (an.z >= an.x && an.z >= an.y) ? vec3(0,1,0) : (an.x >= an.y) ? vec3(0,0,1) : vec3(0,0,1);
    vec3 grad = (axis_u * du + axis_v * dv) * (detail.x + detail.y + detail.z);

    // Only the in-surface part of that slope bends the normal.
    vec3 N = s.normal_ws;
    vec3 g = grad - dot(grad, N) * N;
    s.normal_ws = normalize(N - extra.y * 0.08 * g);

    // Rain: wet darkening wherever the sky reaches, and puddles gathering on flat ground that
    // holds water (gfx/surface/wetness.glsl). Snow needs nothing here -- the G-buffer backbone
    // lays its cover over any surface already.
    //
    // A heightmap terrain measures a hollow from its own grid. A tile world has no grid to read
    // and every top face is dead flat, so the puddle measure is this surface's OWN prefiltered
    // blotch standing in for one: broad patches that sit still under a moving camera, for the
    // same reason the detail above does. Analytic noise here would reintroduce exactly the
    // per-pixel flicker the mip chain was brought in to kill.
#ifdef TERRAIN_PLANET
    float up = frag_uv.y;              // "up" is away from the planet, as styled_kind() reads it
    vec3 geo_n = normalize(s.normal_ws);
#else
    vec3 geo_n = normalize(frag_world_normal);
    float up = geo_n.z;
#endif
    // `patch` is a GLSL keyword (tessellation), hence the name.
    float hollow = clamp(blotch * 0.5 + 0.5, 0.0, 1.0);
    float puddle = k_holds_water[kind] * smoothstep(0.90, 0.99, up) * smoothstep(0.40, 0.85, hollow);
    gfx_wet_apply(s, geo_n, puddle);
}
