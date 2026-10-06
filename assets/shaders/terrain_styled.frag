#version 450

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

#define GFX_SURFACE_FRAGMENT
vec4 styled_sample(sampler2D tex, vec2 uv);
#define GFX_SURFACE_SAMPLE(tex, uv) styled_sample(tex, uv)
#include <gfx/surface/gbuffer_fs.glsl>

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
/// units: all of it well below Nyquist, none of it at Nyquist.
float octave_weight(float freq, float footprint) {
    return 1.0 - smoothstep(0.2, 0.5, freq * footprint);
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
        kind = normalize(frag_world_normal).z > k_lip_threshold ? k0 : k1;
    }
    g_kind = clamp(kind, 0, k_kind_count - 1);
    return g_kind;
}

void gfx_surface_fragment(inout GfxSurface s) {
    int kind = styled_kind();
    vec4 detail = k_detail[kind];
    vec4 extra  = k_extra[kind];

    vec3 p = s.position_ws;
    float footprint = max(length(dFdx(p)), length(dFdy(p)));

    // Each term: the noise value (for albedo) and its world-space gradient (for the bump),
    // already scaled by amplitude and the footprint fade.
    float shade = 0.0;
    vec3  grad  = vec3(0.0);

    float w = octave_weight(k_freq_broad, footprint);
    vec4 n = noise_d(p * k_freq_broad + 17.0);
    shade += detail.x * w * n.x;
    float warm = w * n.x;

    w = octave_weight(k_freq_mid, footprint);
    n = noise_d(p * k_freq_mid + 3.7);
    shade += detail.y * w * n.x;
    grad  += detail.y * w * n.yzw * k_freq_mid;

    w = octave_weight(k_freq_fine, footprint);
    n = noise_d(p * k_freq_fine - 11.0);
    shade += detail.z * w * n.x;
    grad  += detail.z * w * n.yzw * k_freq_fine;

    // Specks: the upper tail of a noise field, softened into rounded flecks -- pebbles in dirt,
    // blade highlights in grass, pits in stone.
    w = octave_weight(k_freq_speck * 2.0, footprint);
    n = noise_d(p * k_freq_speck + 41.0);
    float speck = smoothstep(0.45, 0.75, n.x);
    shade += detail.w * w * speck * extra.w;
    grad  += detail.w * w * extra.w * n.yzw * k_freq_speck * (speck * (1.0 - speck) * 4.0);

    // Strata: noise squashed vertically, so walls band horizontally like layered rock.
    if (extra.x > 0.0) {
        vec3 q = vec3(p.xy * 0.35, p.z * 3.0);
        w = octave_weight(3.0, footprint);
        n = noise_d(q + 7.0);
        shade += extra.x * w * n.x;
        grad  += extra.x * w * n.yzw * vec3(0.35, 0.35, 3.0);
    }

    s.albedo *= clamp(1.0 + shade, 0.5, 1.5);
    s.albedo += extra.z * 0.04 * warm * vec3(1.0, 0.35, -0.6);
    s.albedo = max(s.albedo, vec3(0.0));

    // Bump: tilt the normal against the detail's slope, with the gradient's normal component
    // removed so only the in-surface part bends it.
    vec3 N = s.normal_ws;
    vec3 g = grad - dot(grad, N) * N;
    s.normal_ws = normalize(N - extra.y * 0.08 * g);
}
