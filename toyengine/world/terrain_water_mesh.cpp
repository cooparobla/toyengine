#include <toyengine/world/terrain_water_mesh.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace toy {
namespace world {
namespace {

/**
 * @brief Metres a corner is held under ground that has risen to meet it.
 *
 * Only ever needed as an epsilon against sub-tile interpolation -- the quad between corners
 * interpolates the water's level PLANARLY, while the ground under it is a bilinear sample, so the
 * two can diverge by a hair across one tile even where both read "at the level". A corner whose
 * own ground sits below the level is left untouched (the `min()` below is then a no-op): a flat
 * water surface over ground that is already underwater needs no tuck to stay invisible past the
 * terrain drawing over it.
 */
constexpr float k_smooth_skirt_clearance = 0.2f;

/**
 * @brief Most a corner may drop below the level, metres -- a backstop for a corner whose own
 *        ground genuinely has no apron reach left (the grid's edge, or `water_skirt_tiles`
 *        exhausted on a very gentle shelf), never the normal case.
 */
constexpr float k_smooth_skirt_max_drop = 2.0f;

/** @brief The ground at one GLOBAL corner, in local metres -- exact on a texel boundary for any
 *        `height_sub`, since the bilinear weights there degenerate to that one texel. */
float ground_at_corner(const TerrainField& field, glm::ivec2 corner) {
    const glm::vec2 extent = field.geometry().tile_extent();
    return field.ground_at(glm::vec2(static_cast<float>(corner.x) * extent.x,
                                     static_cast<float>(corner.y) * extent.y));
}

/** @brief The lowest of a tile's four corner samples. */
float lowest_corner_ground(const TerrainField& field, glm::ivec2 tile) {
    float lo = std::numeric_limits<float>::infinity();
    for (int oy = 0; oy <= 1; ++oy) {
        for (int ox = 0; ox <= 1; ++ox) {
            lo = std::min(lo, ground_at_corner(field, glm::ivec2(tile.x + ox, tile.y + oy)));
        }
    }
    return lo;
}

/** @brief Per-build memo of `shore_of()`, keyed by global tile -- a tile's result is read up to
 *        nine times over (once per touching corner's own lookup, and again via each of its four
 *        corners' emitted()/borrow checks), and it is a pure function of the tile alone. */
using ShoreCache = std::unordered_map<std::uint64_t, WaterCell>;

std::uint64_t tile_key(glm::ivec2 tile) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tile.x)) << 32) |
          static_cast<std::uint32_t>(tile.y);
}

/**
 * @brief The water a tile shows: its own, or -- within `skirt_tiles` -- a wet neighbour's.
 *
 * One tile unconditionally (the wet mask is tile-quantised, the waterline it approximates is
 * not), and further only where this tile's ground still has a corner under the neighbour's level
 * -- a steep bank gets no apron past the first tile, a gentle one gets a real one, and the whole
 * tile the waterline actually crosses is always covered (the gate reads the tile's lowest
 * CORNER, not its centre, so a tile whose centre has cleared the level but whose far edge has not
 * is not dropped early). 8-connected (Chebyshev distance), so a diagonal shoreline keeps its
 * corner tiles instead of losing them to a 4-connected dilation.
 *
 * A pure function of the GLOBAL tile, never of which chunk is asking, and its scan order (fixed
 * dy-then-dx, nearest ring first, still water preferred over a river at an equal distance) is
 * fixed too -- what keeps two chunks agreeing on a tile they both touch.
 */
WaterCell shore_of_uncached(const TerrainField& field, glm::ivec2 tile, int skirt_tiles) {
    const WaterCell own = field.water_at(tile);
    if (own.wet()) return own;
    // This tile's own lowest corner never changes across the search below -- hoisted out of the
    // loop rather than recomputed once per candidate ring.
    const float lo_ground = skirt_tiles > 1 ? lowest_corner_ground(field, tile) : 0.0f;
    WaterCell best;
    int best_d = skirt_tiles + 1;
    bool best_is_river = true;
    for (int dy = -skirt_tiles; dy <= skirt_tiles; ++dy) {
        for (int dx = -skirt_tiles; dx <= skirt_tiles; ++dx) {
            const int d = std::max(std::abs(dx), std::abs(dy));
            if (d == 0 || d > skirt_tiles) continue;
            const glm::ivec2 n(tile.x + dx, tile.y + dy);
            if (!field.contains(n)) continue;
            const WaterCell w = field.water_at(n);
            if (!w.wet()) continue;
            // Past the first ring, the apron only continues over ground that is genuinely still
            // under this neighbour's water -- a bank that clears it gets no apron beyond the tile
            // that hides the mask's own quantisation.
            if (d > 1 && lo_ground >= w.level) continue;
            const bool is_river = w.kind == WaterKind::River;
            if (d < best_d || (d == best_d && best_is_river && !is_river)) {
                best = w;
                best_d = d;
                best_is_river = is_river;
            }
        }
    }
    return best_d <= skirt_tiles ? best : WaterCell{};
}

WaterCell shore_of(const TerrainField& field, glm::ivec2 tile, int skirt_tiles, ShoreCache& cache) {
    const std::uint64_t key = tile_key(tile);
    const auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const WaterCell result = shore_of_uncached(field, tile, skirt_tiles);
    cache.emplace(key, result);
    return result;
}

/** @brief Whether `shore_of()` puts a quad over `tile` -- off the grid counts as yes, so the
 *        world's edge never reads as a bank the apron tucks under. */
bool emitted(const TerrainField& field, glm::ivec2 tile, int skirt_tiles, ShoreCache& cache) {
    return !field.contains(tile) || shore_of(field, tile, skirt_tiles, cache).wet();
}

/** @brief What one corner of the water surface stands at. */
struct Corner {
    float level = 0.0f;
    float depth = 0.0f;
    float wave = 1.0f;
    bool  any = false;
};

/**
 * @brief The corner shared by tiles (cx-1..cx, cy-1..cy), addressed by GLOBAL tile coordinate --
 *        never by which chunk or quad is asking, which is what makes two chunks write the same
 *        value for the edge between them.
 *
 * - A corner touching a genuinely wet tile keeps the LOWEST level among the wet tiles touching
 *   it, unchanged by anything below: the visible surface never moves. Lowest, not a mean -- two
 *   different bodies at different levels can share a corner (a river stepping down to a lake, a
 *   lagoon beside the open sea), and only the lower one's level keeps the invariant every water
 *   operation in this file promises, that water never stands above dry ground beside it; a mean
 *   of the two would stand above the lower body's own shore.
 * - A corner touching no wet tile but at least one emitted one (the apron's interior, or its rim)
 *   inherits the lowest level among the bodies that apron continues, the same way, then -- ONLY
 *   where the ground AT THAT EXACT CORNER has risen to meet or pass the level -- is tucked
 *   `k_smooth_skirt_clearance` under that ground, capped at `k_smooth_skirt_max_drop` below the
 *   level. A corner whose own ground is still below the level is left exactly at the level: the
 *   terrain already draws over it there, so dropping it toward that ground would only carve a
 *   visible ramp through open water the surface was already hidden behind.
 *
 * Depth (the wave attenuation input) is the level less the ground AT THIS CORNER -- exact, and
 * zero precisely at the waterline, rather than a mean over the tiles touching it.
 */
Corner corner_at(const TerrainField& field, int cx, int cy, int skirt_tiles, float wave_scale_river,
                 ShoreCache& cache) {
    Corner out;
    const glm::ivec2 touch[4] = {{cx - 1, cy - 1}, {cx, cy - 1}, {cx - 1, cy}, {cx, cy}};

    float lowest_level = std::numeric_limits<float>::infinity();
    float lowest_wave = 1.0f;
    int wet_n = 0;
    bool any_emitted = false;

    for (const glm::ivec2& t : touch) {
        if (field.contains(t)) {
            const WaterCell own = field.water_at(t);
            if (own.wet()) {
                if (own.level < lowest_level) {
                    lowest_level = own.level;
                    lowest_wave = own.kind == WaterKind::River ? wave_scale_river : 1.0f;
                }
                ++wet_n;
            }
        }
        if (emitted(field, t, skirt_tiles, cache)) any_emitted = true;
    }

    if (!any_emitted) return out;   // no touching tile carries water at all: not part of the mesh
    out.any = true;

    float level;
    if (wet_n > 0) {
        level = lowest_level;
        out.wave = lowest_wave;
    } else {
        // The apron's interior: no touching tile is truly wet, but at least one is emitted, so it
        // continues a body found by widening the search exactly as the tile itself did.
        float lvl = std::numeric_limits<float>::infinity();
        float w = 1.0f;
        for (const glm::ivec2& t : touch) {
            if (!field.contains(t)) continue;
            const WaterCell borrowed = shore_of(field, t, skirt_tiles, cache);
            if (!borrowed.wet()) continue;
            if (borrowed.level < lvl) {
                lvl = borrowed.level;
                w = borrowed.kind == WaterKind::River ? wave_scale_river : 1.0f;
            }
        }
        if (lvl == std::numeric_limits<float>::infinity()) return Corner{};   // every in-bounds touching tile is itself unemitted
        level = lvl;
        out.wave = w;
    }

    const float ground = ground_at_corner(field, glm::ivec2(cx, cy));
    out.depth = std::max(0.0f, level - ground);

    // Skipped when a touching tile is truly wet (that corner's level is the visible waterline and
    // must not move) AND wherever this corner's own ground is still under the level -- a flat
    // surface there is already invisible behind the terrain, so there is nothing to tuck and
    // dropping it anyway would carve a visible ramp through open water instead of staying flat.
    if (wet_n == 0 && ground >= level) {
        level = std::max(level - k_smooth_skirt_max_drop, std::min(level, ground - k_smooth_skirt_clearance));
    }
    out.level = level;
    return out;
}

/** @brief One mesh under construction, with its corner-to-vertex table. */
struct Builder {
    ChunkWaterData* out = nullptr;
    std::unordered_map<std::uint64_t, std::uint32_t> corners;

    /** @brief The vertex for a global corner, added once per mesh so a quad edge is shared. */
    std::uint32_t vertex(const TerrainField& field, int cx, int cy, int skirt_tiles, const glm::vec2& local,
                         float wave_scale_river, ShoreCache& cache) {
        const std::uint64_t key = tile_key(glm::ivec2(cx, cy));
        const auto it = corners.find(key);
        if (it != corners.end()) return it->second;
        const Corner c = corner_at(field, cx, cy, skirt_tiles, wave_scale_river, cache);
        const std::uint32_t index = static_cast<std::uint32_t>(out->positions.size());
        out->positions.push_back(glm::vec3(local.x, local.y, c.level));
        out->depths.push_back(c.depth);
        out->wave_scales.push_back(c.wave);
        out->uvs.push_back(glm::vec2(local.x, local.y));
        corners.emplace(key, index);
        return index;
    }
};

} // namespace

void build_smooth_chunk_water(const TerrainField& field, glm::ivec2 chunk, int chunk_tiles,
                              float wave_scale_river, int skirt_tiles, ChunkWaterData& still,
                              ChunkWaterData& flowing) {
    still.clear();
    flowing.clear();
    if (field.mode() != TerrainMode::Heightmap || field.empty()) return;
    const int skirt = std::max(1, skirt_tiles);

    const int ct = std::max(1, chunk_tiles);
    const glm::vec2 extent = field.geometry().tile_extent();
    const glm::vec2 chunk_origin(static_cast<float>(chunk.x * ct) * extent.x,
                                 static_cast<float>(chunk.y * ct) * extent.y);

    Builder still_b{&still, {}};
    Builder flow_b{&flowing, {}};
    // One memo of shore_of() for the whole chunk: it is a pure function of the global tile and is
    // otherwise read up to nine times over per tile (once directly, and again through each of its
    // four corners' emitted()/borrow checks).
    ShoreCache cache;

    for (int ty = chunk.y * ct; ty < (chunk.y + 1) * ct; ++ty) {
        for (int tx = chunk.x * ct; tx < (chunk.x + 1) * ct; ++tx) {
            const glm::ivec2 tile(tx, ty);
            if (!field.contains(tile)) continue;
            // Wet tiles, and the apron: ground that still carries that neighbour's water, out to
            // `skirt`. Without it the surface would stop at a tile boundary rather than at the
            // waterline.
            const WaterCell shows = shore_of(field, tile, skirt, cache);
            if (!shows.wet()) continue;

            Builder& b = shows.kind == WaterKind::River ? flow_b : still_b;
            std::uint32_t v[4];
            for (int c = 0; c < 4; ++c) {
                const int ox = c == 1 || c == 2 ? 1 : 0;
                const int oy = c >= 2 ? 1 : 0;
                const glm::vec2 local(static_cast<float>(tx + ox) * extent.x - chunk_origin.x,
                                      static_cast<float>(ty + oy) * extent.y - chunk_origin.y);
                v[c] = b.vertex(field, tx + ox, ty + oy, skirt, local, wave_scale_river, cache);
            }
            // Two triangles, anticlockwise seen from above (+Z up), as the tile path emits them.
            b.out->indices.insert(b.out->indices.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        }
    }

    // A river's `u` runs downstream (water_flow_bake.h's contract). The flow is a per-tile
    // octant, so the direction is taken once for the chunk from the river tiles in it: a chunk
    // is small beside a river's bends, and one direction per chunk keeps the seam consistent.
    if (!flowing.empty()) {
        glm::vec2 downstream(0.0f);
        for (int ty = chunk.y * ct; ty < (chunk.y + 1) * ct; ++ty) {
            for (int tx = chunk.x * ct; tx < (chunk.x + 1) * ct; ++tx) {
                const glm::ivec2 tile(tx, ty);
                if (!field.contains(tile)) continue;
                const WaterCell w = field.water_at(tile);
                if (w.kind != WaterKind::River || w.flow < 1 || w.flow > 8) continue;
                downstream += flow_direction(w.flow);
            }
        }
        if (glm::length(downstream) > 1e-4f) {
            downstream = glm::normalize(downstream);
            const glm::vec2 across(-downstream.y, downstream.x);
            for (std::size_t i = 0; i < flowing.uvs.size(); ++i) {
                const glm::vec2 p = flowing.uvs[i];
                flowing.uvs[i] = glm::vec2(glm::dot(p, downstream), glm::dot(p, across));
            }
        }
    }
}

} // namespace world
} // namespace toy
