/**
 * @file terrain_shore.cpp
 * @brief See terrain_shore.h.
 */

#include <toyengine/world/terrain_shore.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include <coopa/maps/biome.h>

#include <toyengine/mapgen/terrain_sampler.h>
#include <toyengine/world/terra_data.h>

namespace toy {
namespace world {

namespace {

constexpr float k_inf = std::numeric_limits<float>::infinity();
/** @brief Sentinel for shore_floor_(): no wet 8-neighbour, so nothing floors this tile. */
constexpr float k_no_floor = -1.0e9f;

enum class Cone { Ceiling, Floor };

/** @brief One source a cone propagates from: a tile, and the height its cone starts at there. */
struct Seed {
    std::int32_t x = 0, y = 0;
    float height = 0.0f;
};

/**
 * @brief The 8-neighbour step every pass here shares, exactly as terrain_water.cpp takes it:
 *        through `topology` when there is one, plain arithmetic with a bounds reject when there
 *        is not. Null is the flat grid.
 * @return false when the step leaves a flat grid, so the caller skips that neighbour.
 */
bool step_(const TileGrid* topology, std::int32_t w, std::int32_t h, std::int32_t x, std::int32_t y,
           int dx, int dy, glm::ivec2& out) {
    if (topology != nullptr) {
        out = topology->neighbour(x, y, dx, dy);
        return true;
    }
    const std::int32_t nx = x + dx;
    const std::int32_t ny = y + dy;
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) return false;
    out = {nx, ny};
    return true;
}

/**
 * @brief `settle_water_rounding()`'s own bank value for a dry tile: the max water terrace over its
 *        8 neighbours, stepped through `topology` exactly as that pass does. `k_no_floor` when
 *        none of the 8 neighbours are wet.
 *
 * Reusing this exact expression (rather than re-deriving it) is what lets the fold clamp a dry
 * waterline tile to precisely what the rounding already set it to -- so a tile adjacent to both a
 * river at one terrace and a lake at a higher one still floors at the lake's, never at the
 * river's, the same way the rounding itself does.
 */
float shore_floor_(const TerraData& d, const TileGrid* topology, std::int32_t w, std::int32_t h,
                   std::int32_t x, std::int32_t y, float hstep) {
    float bank = k_no_floor;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            glm::ivec2 n;
            if (!step_(topology, w, h, x, y, dx, dy, n)) continue;
            const TileCell& o = d.get(n.x, n.y);
            if (o.wet()) bank = std::max(bank, std::round(o.water_level / hstep));
        }
    }
    return bank;
}

/** @brief The engine's TileCell::water_kind as mapcoopa's own WaterKind, for bed_kind_for(). */
coopa::maps::WaterKind to_map_water_kind_(WaterKind k) {
    switch (k) {
        case WaterKind::Ocean: return coopa::maps::WaterKind::Sea;
        case WaterKind::Lake:  return coopa::maps::WaterKind::Lake;
        case WaterKind::River: return coopa::maps::WaterKind::River;
        default:               return coopa::maps::WaterKind::None;
    }
}

/**
 * @brief Where a tile's centre sits on the terrain's real surface, for the dither's lattice.
 *
 * Through `TileGrid::grid_point()` when there is a topology, which is what keeps the lattice
 * continuous across a region's edge: two tiles physically adjacent there land next to each other
 * in space however far apart their storage coordinates are. Flat, the same thing is just the tile
 * grid scaled by `tile_size`.
 */
glm::vec3 surface_point_(const TerrainParams& params, const TileGrid* topology, std::int32_t x,
                         std::int32_t y) {
    if (topology != nullptr) {
        return topology->grid_point(params, static_cast<double>(x) + 0.5,
                                    static_cast<double>(y) + 0.5, 0.0, -1);
    }
    return glm::vec3(static_cast<float>(static_cast<double>(x) + 0.5) * params.tile_size,
                     static_cast<float>(static_cast<double>(y) + 0.5) * params.tile_size, 0.0f);
}

/**
 * @brief A per-tile dither, [0, 1), constant over `period_tiles`-wide cells of a lattice fixed in
 *        the terrain's own space rather than in tile storage.
 *
 * One constant value per lattice cell (rather than an interpolated noise) is deliberate: it reads
 * as a terrace edge wandering in short lobes, not as a smooth ramp that would hide the very
 * terracing it is meant to soften.
 */
float jitter01_(const glm::vec3& at, float period_tiles, float tile_size, std::uint32_t seed) {
    if (period_tiles <= 0.0f || tile_size <= 0.0f) return 0.5f;
    const double scale = 1.0 / (static_cast<double>(period_tiles) * static_cast<double>(tile_size));
    const auto cell = [&](double v) { return static_cast<std::int32_t>(std::floor(v * scale)); };
    std::uint32_t hv = static_cast<std::uint32_t>(cell(at.x)) * 374761393u;
    hv ^= static_cast<std::uint32_t>(cell(at.y)) * 668265263u;
    hv ^= static_cast<std::uint32_t>(cell(at.z)) * 2246822519u;
    hv ^= seed;
    hv = (hv ^ (hv >> 13)) * 1274126177u;
    hv ^= hv >> 16;
    return static_cast<float>(hv & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

/** @brief Keyed on the flat tile index rather than glm::ivec2: std::greater<Node> needs an ordering
 *         on the whole pair, and glm's vector types don't give operator> a scalar bool to return. */
using Node = std::pair<float, std::size_t>;

/**
 * @brief The per-tile distance and the open set a cone walks, carried across all four cone calls.
 *
 * A large grid makes `dist` alone tens of megabytes, so allocating and freeing it per cone is pure
 * waste; one scratch hands it round.
 */
struct ConeScratch {
    std::vector<float> dist;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
};

/** @brief One grading cone's shape; see ShoreProfile (terrain_params.h) for what each means. */
struct ConeShape {
    float slope = 0.0f;        /**< @brief Recovery slope at the surface (`*_slope_steps`). */
    float taper_slope = 0.0f;  /**< @brief Recovery slope with the budget spent, and the riser cap. */
    float max_delta = 0.0f;    /**< @brief The budget: the most a tile may move (`*_max_steps`). */
    float reach_tiles = 0.0f;
    /**
     * @brief Whether a step's cost carries the ground's own rise between the two tiles.
     *
     * True for the cut and the fill, which grade a SURFACE that has to blend into the terrain
     * around it: without the term they excavate (or buttress) every slope steeper than themselves.
     * False for the bed, which fills a submerged hole bounded above by its own water terrace and
     * has nothing to blend into -- there the term would make the cone descend into the hole as
     * fast as the hole deepens, so it would never rise above the bed and would do nothing at all.
     */
    bool follow_ground = true;
    /**
     * @brief Measure `depth` below the tile's own WATER TERRACE rather than from its baked height.
     *
     * The bed cone's only correct choice, and not a cosmetic one: it is the cone that RAISES the
     * ground it is measured against, so depth-from-ground shrinks on a second pass, the recovery
     * slope shrinks with it, the cone descends more slowly and the bed comes up again -- it
     * ratchets, and grading stops being a fixed point. A water terrace is invariant under this
     * pass, so measuring from it is stable. It is also the more natural parameter: a bed cone is
     * filling toward that terrace, and "how far under the water am I" is what should decide how
     * steeply it shelves.
     */
    bool depth_below_water = false;
};

/**
 * @brief A grading cone: from every seed, the height the shore grades to at each tile, stopping
 *        where it meets the baked ground.
 *
 * Per step `p -> q` the cone pays
 *
 *     rise = clamp( (ground(q) - ground(p)) + recovery(depth(p)) * step,  0,  taper_slope * step )
 *
 * for a Ceiling (mirrored for a Floor), where `depth(p)` is how far the cone has already moved `p`
 * and `recovery` ramps linearly from `slope` at depth 0 to `taper_slope` at depth `max_delta`. An
 * edge is taken only while all of:
 *
 * - **the ground prune** -- the cone is still clear of the ground. `min(baked, cone)` is exactly
 *   `baked` where they meet, so stopping there introduces no riser at all.
 * - **the budget refusal** -- the candidate would not move `q` more than `max_delta` from its
 *   baked height. This is the per-tile bound expressed as a refusal to ENTER rather than as a
 *   clamped value: a clamped value would be `baked`-relative, and re-grading already-graded ground
 *   would then shave another `max_delta` off every pass instead of being a fixed point.
 * - the ordinary Dijkstra improvement, and `reach_tiles`.
 *
 * **Why this is still an exact Dijkstra.** The step cost depends on the tail's own value, so the
 * usual constant-weight argument does not apply directly; the generalisation does. An edge
 * function `f(x)` admits Dijkstra when `f(x) >= x` and `f` is non-decreasing. The first holds
 * because `rise >= 0`. For the second, `recovery` falls by `(taper_slope - slope)/max_delta` per
 * unit of value, so `f' = 1 - (taper_slope - slope) * step / max_delta >= 0` exactly when
 * `max_delta >= (taper_slope - slope) * step` -- which is why the caller must satisfy that, and
 * why `recovery` ramps linearly instead of switching between two regimes: a hard switch is
 * non-monotone at the jump and would make the result a heuristic rather than the true cone. The
 * prune and the refusal only delete edges, each decided from the tail's final value and fixed
 * data, so the min-over-every-seed property the multi-source form exists for -- correct at a river
 * mouth, a confluence, a lake beside a river -- is preserved.
 *
 * @param out Full-grid buffer, `+-infinity` where no seed's cone reached a tile.
 */
void grade_cone_(const TerraData& d, const TileGrid* topology, const std::vector<Seed>& seeds,
                 Cone sense, const ConeShape& shape, float hstep, ConeScratch& s,
                 std::vector<float>& out) {
    const std::int32_t w = d.geometry().tiles_x;
    const std::int32_t h = d.geometry().tiles_y;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    out.assign(count, sense == Cone::Ceiling ? k_inf : -k_inf);
    if (seeds.empty() || shape.slope <= 0.0f || shape.max_delta <= 0.0f) return;

    s.dist.assign(count, k_inf);
    const auto index_of = [w](std::int32_t x, std::int32_t y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    };
    const auto ground = [&d](std::int32_t x, std::int32_t y) {
        return static_cast<float>(d.get(x, y).steps);
    };
    // A min-heap keyed by `sign * value` serves both senses: Ceiling wants the smallest value
    // finalized first, Floor the largest, and negating the key for Floor turns that into the
    // same comparison.
    const float sign = sense == Cone::Ceiling ? 1.0f : -1.0f;

    for (const Seed& seed : seeds) {
        const std::size_t idx = index_of(seed.x, seed.y);
        if (s.dist[idx] == 0.0f) continue;  // a duplicate seed at the same tile keeps the first
        float v = seed.height;
        if (sense == Cone::Ceiling) {
            // The budget binds the seed too. A shoreline tile standing far above its own water is
            // the FOOT OF A CLIFF, and cutting it straight down to the terrace would only move the
            // cliff one tile inland. A Floor seed needs no such guard: settle_water_rounding() has
            // already raised every dry waterline tile to its bank, so a fill starts at depth zero.
            v = std::max(v, ground(seed.x, seed.y) - shape.max_delta);
        }
        s.dist[idx] = 0.0f;
        out[idx] = v;
        s.open.push({sign * v, idx});
    }

    while (!s.open.empty()) {
        const Node top = s.open.top();
        s.open.pop();
        const float value = sign * top.first;
        const std::size_t pidx = top.second;
        // Lazy deletion: a better value for this tile was pushed after this entry.
        if (sense == Cone::Ceiling ? value > out[pidx] : value < out[pidx]) continue;
        const float pdist = s.dist[pidx];
        if (pdist >= shape.reach_tiles) continue;
        const glm::ivec2 p{static_cast<std::int32_t>(pidx % static_cast<std::size_t>(w)),
                           static_cast<std::int32_t>(pidx / static_cast<std::size_t>(w))};
        const float gp = ground(p.x, p.y);
        float depth;
        if (shape.depth_below_water) {
            const TileCell& cp = d.get(p.x, p.y);
            depth = cp.wet() ? std::max(0.0f, std::round(cp.water_level / hstep) - value) : 0.0f;
        } else {
            depth = sense == Cone::Ceiling ? std::max(0.0f, gp - value) : std::max(0.0f, value - gp);
        }
        const float recovery = shape.slope + (shape.taper_slope - shape.slope) *
                                                 std::min(depth / shape.max_delta, 1.0f);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                glm::ivec2 n;
                if (!step_(topology, w, h, p.x, p.y, dx, dy, n)) continue;
                const std::size_t nidx = index_of(n.x, n.y);
                const float step = (dx != 0 && dy != 0) ? 1.41421356f : 1.0f;
                const float nd = pdist + step;
                if (nd > shape.reach_tiles) continue;
                const float gq = ground(n.x, n.y);
                const float climb = shape.follow_ground
                                        ? (sense == Cone::Ceiling ? gq - gp : gp - gq)
                                        : 0.0f;
                const float rise = std::clamp(climb + recovery * step, 0.0f, shape.taper_slope * step);
                const float candidate = sense == Cone::Ceiling ? value + rise : value - rise;
                // Met the ground: the fold would take the baked height here anyway, and there is
                // no riser to answer for past it.
                if (sense == Cone::Ceiling ? candidate >= gq : candidate <= gq) continue;
                // Over budget: this tile is a cliff face, not a bank, so leave it standing.
                if (sense == Cone::Ceiling ? candidate < gq - shape.max_delta
                                           : candidate > gq + shape.max_delta) continue;
                const bool better = sense == Cone::Ceiling ? candidate < out[nidx] : candidate > out[nidx];
                if (!better) continue;
                out[nidx] = candidate;
                s.dist[nidx] = nd;
                s.open.push({sign * candidate, nidx});
            }
        }
    }
}

/**
 * @brief Chamfer distance to the nearest wet tile, and that tile's water terrace carried along
 *        with it -- the two things the beach band needs.
 *
 * Not a grading cone and deliberately not built from one: at slope 1 a cone's value IS the chamfer
 * distance, so a ground prune would be meaningless here, and the band really is a distance.
 *
 * The terrace rides along as an ordinary nearest-seed attribute, copied from the tile that
 * improved this one. That is exact because the field is a true distance field: the tile a value
 * arrives from is on a shortest path back to the seed that owns it.
 *
 * @param terrace Receives the nearest water's terrace in whole steps; `k_no_floor` where unreached.
 */
void coast_distance_(const TerraData& d, const TileGrid* topology, const std::vector<Seed>& seeds,
                     float max_dist, ConeScratch& s, std::vector<float>& out,
                     std::vector<float>& terrace) {
    const std::int32_t w = d.geometry().tiles_x;
    const std::int32_t h = d.geometry().tiles_y;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    out.assign(count, k_inf);
    terrace.assign(count, k_no_floor);
    if (seeds.empty() || max_dist <= 0.0f) return;
    const auto index_of = [w](std::int32_t x, std::int32_t y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
    };
    for (const Seed& seed : seeds) {
        const std::size_t idx = index_of(seed.x, seed.y);
        if (out[idx] == 0.0f) continue;
        out[idx] = 0.0f;
        terrace[idx] = seed.height;  // the wet tile's own terrace, in steps
        s.open.push({0.0f, idx});
    }
    while (!s.open.empty()) {
        const Node top = s.open.top();
        s.open.pop();
        if (top.first > out[top.second]) continue;  // lazy deletion
        if (top.first >= max_dist) continue;
        const glm::ivec2 p{static_cast<std::int32_t>(top.second % static_cast<std::size_t>(w)),
                           static_cast<std::int32_t>(top.second / static_cast<std::size_t>(w))};
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                glm::ivec2 n;
                if (!step_(topology, w, h, p.x, p.y, dx, dy, n)) continue;
                const std::size_t nidx = index_of(n.x, n.y);
                const float nd = top.first + ((dx != 0 && dy != 0) ? 1.41421356f : 1.0f);
                if (nd > max_dist || nd >= out[nidx]) continue;
                out[nidx] = nd;
                terrace[nidx] = terrace[top.second];
                s.open.push({nd, nidx});
            }
        }
    }
}

} // namespace

void grade_tile_shore(TerrainField& field, const IRect& rect, const TerrainParams& params,
                      const TileGrid* topology) {
    const ShoreProfile& profile = params.shore;
    if (!profile.enabled) return;
    TerraData* d = field.data();
    if (d == nullptr) return;
    // A field with a height plane is grade_water_shore()'s, not this pass's: there is no terrace
    // to grade, and writing `steps` under a height plane would only be overwritten from it.
    if (d->has_heights()) return;
    const float hstep = d->geometry().height_step;
    if (hstep <= 0.0f) return;
    const std::int32_t w = d->geometry().tiles_x;
    const std::int32_t h = d->geometry().tiles_y;
    const IRect limit = rect.clipped(w, h);
    if (limit.empty()) return;
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);

    // --- Seeds, and the exact settle_water_rounding() bank value for every dry tile. ---
    std::vector<Seed> shore_seeds;  // dry, >= 1 wet neighbour: the cut and fill cones share these.
    std::vector<Seed> bed_seeds;    // wet, >= 1 dry neighbour: the bed cone's seeds.
    std::vector<Seed> coast_seeds;  // every wet tile, at its terrace: the beach band's distance.
    std::vector<float> shore_floor(count, k_no_floor);

    for (std::int32_t y = limit.y0; y < limit.y1; ++y) {
        for (std::int32_t x = limit.x0; x < limit.x1; ++x) {
            const TileCell& c = d->at(x, y);
            if (c.wet()) {
                // Seeded with its own terrace, which coast_distance_() carries out to every tile
                // in the beach band as the height of the water nearest it.
                if (profile.retexture) coast_seeds.push_back({x, y, std::round(c.water_level / hstep)});
                bool dry_adjacent = false;
                for (int dy = -1; dy <= 1 && !dry_adjacent; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        glm::ivec2 n;
                        if (!step_(topology, w, h, x, y, dx, dy, n)) continue;
                        if (!d->get(n.x, n.y).wet()) { dry_adjacent = true; break; }
                    }
                }
                if (dry_adjacent) bed_seeds.push_back({x, y, static_cast<float>(c.steps)});
            } else {
                const float bank = shore_floor_(*d, topology, w, h, x, y, hstep);
                shore_floor[static_cast<std::size_t>(y) * w + x] = bank;
                if (bank > k_no_floor) shore_seeds.push_back({x, y, bank});
            }
        }
    }

    // --- The grading cones. ---
    ConeScratch scratch;
    std::vector<float> cut, fill, bed, coast, coast_terrace;
    const auto shape = [&profile](float slope, float taper, float max_delta, bool follow, bool below_water) {
        ConeShape c;
        c.slope = slope;
        // grade_cone_()'s Dijkstra is exact only while `max_delta >= (taper - slope) * step`, and
        // the longest step is a diagonal. Capping the taper here rather than asserting keeps a
        // hand-tuned profile honest instead of silently returning something that is not the cone.
        c.taper_slope = std::max(slope, std::min(taper, slope + max_delta / 1.41421356f));
        c.max_delta = max_delta;
        c.reach_tiles = profile.cone_reach_tiles;
        c.follow_ground = follow;
        c.depth_below_water = below_water;
        return c;
    };
    grade_cone_(*d, topology, shore_seeds, Cone::Ceiling,
                shape(profile.cut_slope_steps, profile.cut_taper_slope_steps, profile.cut_max_steps, true, false),
                hstep, scratch, cut);
    grade_cone_(*d, topology, shore_seeds, Cone::Floor,
                shape(profile.fill_slope_steps, profile.fill_taper_slope_steps, profile.fill_max_steps, true, false),
                hstep, scratch, fill);
    grade_cone_(*d, topology, bed_seeds, Cone::Floor,
                shape(profile.bed_slope_steps, profile.bed_taper_slope_steps, profile.bed_max_steps, false, true),
                hstep, scratch, bed);
    if (profile.retexture) {
        coast_distance_(*d, topology, coast_seeds, profile.beach_tiles + profile.beach_jitter_tiles,
                        scratch, coast, coast_terrace);
    }

    // --- Fold, quantise, write; then retexture. ---
    for (std::int32_t y = limit.y0; y < limit.y1; ++y) {
        for (std::int32_t x = limit.x0; x < limit.x1; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * w + x;
            TileCell& c = d->at(x, y);
            // Pure function of (x, y): the same tile dithers the same way every time this runs,
            // which is what makes an already-graded tile a fixed point.
            const float j = jitter01_(surface_point_(params, topology, x, y), profile.jitter_tiles,
                                      d->geometry().tile_size, profile.jitter_seed) *
                            profile.jitter_steps;

            if (c.wet()) {
                const float terrace = std::round(c.water_level / hstep);
                float t = static_cast<float>(c.steps);
                t = std::max(t, bed[idx]);  // shelve a deep bed up toward its margin
                std::int32_t q = static_cast<std::int32_t>(std::floor(t + j));
                q = std::min(q, static_cast<std::int32_t>(terrace) - 1);  // never above the bed's own cap
                if (q == c.steps) continue;
                c.steps = static_cast<std::int16_t>(q);
                if (profile.retexture) {
                    const std::int32_t depth = static_cast<std::int32_t>(terrace) - q;
                    c.set_top_kind(TerrainSampler::bed_kind_for(
                        to_map_water_kind_(static_cast<WaterKind>(c.water_kind)), std::max(1, depth)));
                    c.set_side_kind(side_kind_for(c.top_kind()));
                }
            } else {
                float t = static_cast<float>(c.steps);
                t = std::min(t, cut[idx]);   // cut a bank back toward a walkable grade
                t = std::max(t, fill[idx]);  // fill the lip settle_water_rounding() leaves
                std::int32_t q = static_cast<std::int32_t>(std::floor(t + j));
                // The water invariant: never lower a tile below what settle_water_rounding()
                // already pinned it to -- max over every wet 8-neighbour, not the nearest one.
                if (shore_floor[idx] > k_no_floor) {
                    q = std::max(q, static_cast<std::int32_t>(std::lround(shore_floor[idx])));
                }
                if (q != c.steps) c.steps = static_cast<std::int16_t>(q);

                if (profile.retexture && !coast.empty() &&
                    coast[idx] <= profile.beach_tiles + profile.beach_jitter_tiles) {
                    // A smoothstep-free threshold: beach_jitter_tiles already widened the coast
                    // field's own reach, so a hard compare against the (now dithered-by-distance)
                    // band edge is enough -- a second independent jitter would only double-dip.
                    //
                    // Measured against the NEAREST WATER'S TERRACE, never against the cut cone.
                    // Reading the cut made the test answer itself: on any tile the cut bound, `q`
                    // IS floor(cut), so `q - local_shore` was ~0 and every graded tile within the
                    // band sanded over whatever its real height above the water.
                    const float local_shore = coast_terrace[idx];
                    const bool low_enough = local_shore > k_no_floor &&
                        (static_cast<float>(q) - local_shore) <= static_cast<float>(profile.beach_rise_steps);
                    if (low_enough) {
                        const TileKind top = c.top_kind();
                        if (top == TileKind::Grass || top == TileKind::Moss || top == TileKind::Clay ||
                            top == TileKind::Ash) {
                            c.set_top_kind(TileKind::Sand);
                            c.set_side_kind(side_kind_for(TileKind::Sand));
                            c.biome = static_cast<std::uint8_t>(coopa::maps::Biome::Beach);
                        }
                    }
                }
            }
        }
    }
}

} // namespace world
} // namespace toy
