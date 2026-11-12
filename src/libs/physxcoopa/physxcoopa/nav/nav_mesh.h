/**
 * @file nav_mesh.h
 * @brief NavMesh -- an immutable snapshot of one agent profile's walkable surface -- with its
 *        spatial queries (nearest span, floor height, grid raycast, move-along-surface).
 *
 * Immutability is the concurrency model. Tiles are shared_ptr<const NavTile>; committing
 * rebuilt tiles (NavMesh::commit()) produces a NEW NavMesh that shares every untouched tile
 * with the old one and copies-on-write the few neighbours whose border links change. Path
 * queries and background flow-field builds hold a shared_ptr<const NavMesh> for as long as they
 * run, so a rebuild landing mid-query can never change what they read.
 *
 * Graph structure:
 *   - fine graph: spans, linked to their 4 neighbours (NavSpan::link) -- diagonals are derived
 *     (both L-shaped routes must exist and agree, so corners are never cut) -- plus directed
 *     off-mesh links (jumps, ladders, drops).
 *   - coarse graph: regions (tile-local connected patches), linked across tile borders and by
 *     off-mesh links; A* plans a corridor over it before the fine search (path_query.h).
 *   - components: connected components of the coarse graph, for an O(1) "is the goal
 *     reachable at all" check before any search runs.
 */

#ifndef PHYSXCOOPA_NAV_NAV_MESH_H
#define PHYSXCOOPA_NAV_NAV_MESH_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_tile.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/**
 * @struct OffMeshLink
 * @brief A hand-placed connection the voxel surface can't express: a jump, a ladder, a drop.
 */
struct OffMeshLink {
    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f};
    bool bidirectional = true;
    /** @brief Cost multiplier on the link's straight-line length. */
    float cost = 1.0f;
    uint8_t area = k_area_walkable;
    /** @brief Caller's identifier, carried through to path results. */
    uint32_t user_id = 0;
    /** @brief How far from each endpoint to look for the surface it attaches to, metres. */
    float snap_radius = 1.0f;

    // Resolved against the mesh at commit time:
    SpanRef start_ref = k_invalid_span;
    SpanRef end_ref = k_invalid_span;
};

/** @brief A directed traversal of an off-mesh link, from the span it leaves. */
struct LinkEdge {
    uint16_t link = 0;
    SpanRef to = k_invalid_span;
    bool reverse = false; /**< Traverses end -> start. */
};

/** @brief Result of NavMesh::raycast(). */
struct NavRaycastHit {
    bool blocked = false;
    float t = 1.0f;                 /**< Fraction of start->end travelled before the block. */
    glm::vec3 position{0.0f};       /**< Where it stopped, on the surface. */
    glm::vec2 normal{0.0f};         /**< XY normal of the edge hit (zero if not blocked). */
    SpanRef last = k_invalid_span;  /**< Last span reached. */
};

/** @brief Result of NavMesh::move_along_surface(). */
struct NavMoveResult {
    glm::vec3 position{0.0f};
    SpanRef ref = k_invalid_span;
    bool blocked = false;
};

/**
 * @class NavMesh
 * @brief One agent profile's walkable surface. See the file doc.
 */
class NavMesh {
public:
    NavGridParams params;
    AreaTable areas;
    uint64_t version = 0;

    /** @brief Per tile index (ty * tiles_x + tx); nullptr for a tile with no walkable span. */
    std::vector<std::shared_ptr<const NavTile>> tiles;
    /** @brief Per tile, per region: connected-component id. */
    std::vector<std::vector<uint32_t>> components;
    std::vector<OffMeshLink> links;
    /** @brief Outgoing link traversals per span. */
    std::unordered_map<SpanRef, std::vector<LinkEdge>> link_edges;
    /** @brief Coarse-graph edges contributed by off-mesh links, per region. */
    std::unordered_map<RegionRef, std::vector<RegionEdge>> link_region_edges;
    /** @brief Whether any resolved link is one-way. Components ignore one-way links (they are
     *         symmetric), so with one present a component mismatch no longer proves a goal
     *         unreachable. */
    bool has_one_way_links = false;

    // --- Construction -----------------------------------------------------------------------

    /** @brief An empty mesh on `params`' grid. */
    static std::shared_ptr<NavMesh> make_empty(const NavGridParams& params, const AreaTable& areas) {
        auto m = std::make_shared<NavMesh>();
        m->params = params;
        m->areas = areas;
        m->tiles.resize(params.tile_count());
        m->components.resize(params.tile_count());
        return m;
    }

    /**
     * @brief Produces the mesh that results from replacing the given tiles (nullptr clears a
     *        tile) and the off-mesh link set. `prev` is untouched. Runs the stitch step for the
     *        replaced tiles' borders, then recomputes link resolution and components.
     */
    static std::shared_ptr<const NavMesh> commit(const NavMesh& prev,
                                                 std::vector<std::pair<uint32_t, std::shared_ptr<NavTile>>> replaced,
                                                 const std::vector<OffMeshLink>& link_set) {
        auto m = std::make_shared<NavMesh>();
        m->params = prev.params;
        m->areas = prev.areas;
        m->version = prev.version + 1;
        m->tiles = prev.tiles;

        const NavGridParams& p = m->params;
        // Mutable working set: replaced tiles plus copy-on-write copies of their neighbours.
        std::unordered_map<uint32_t, std::shared_ptr<NavTile>> touched;
        for (auto& [index, tile] : replaced) {
            touched[index] = tile;
            m->tiles[index] = tile;
        }
        for (auto& [index, tile] : replaced) {
            int tx = static_cast<int>(index % static_cast<uint32_t>(p.tiles_x));
            int ty = static_cast<int>(index / static_cast<uint32_t>(p.tiles_x));
            for (int d = 0; d < 4; ++d) {
                int nx = tx + k_dir_dx[d], ny = ty + k_dir_dy[d];
                if (nx < 0 || ny < 0 || nx >= p.tiles_x || ny >= p.tiles_y) continue;
                uint32_t ni = static_cast<uint32_t>(nx + ny * p.tiles_x);
                if (touched.count(ni) || !m->tiles[ni]) continue;
                auto copy = std::make_shared<NavTile>(*m->tiles[ni]);
                touched[ni] = copy;
                m->tiles[ni] = copy;
            }
        }
        // Stitch every border of every touched tile (idempotent, so shared borders done twice
        // just agree), then rebuild those tiles' region edges.
        for (auto& [index, tile] : touched) {
            if (tile) m->stitch_tile_(*tile);
        }
        for (auto& [index, tile] : touched) {
            if (tile) m->build_region_edges_(*tile);
        }
        m->resolve_links_(link_set);
        m->build_components_();
        return m;
    }

    // --- Basic access -----------------------------------------------------------------------

    uint32_t tile_index(int tx, int ty) const { return static_cast<uint32_t>(tx + ty * params.tiles_x); }

    const NavTile* tile_at(int tx, int ty) const {
        if (tx < 0 || ty < 0 || tx >= params.tiles_x || ty >= params.tiles_y) return nullptr;
        return tiles[tile_index(tx, ty)].get();
    }

    const NavTile* tile_of(SpanRef r) const {
        uint32_t t = ref_tile(r);
        return t < tiles.size() ? tiles[t].get() : nullptr;
    }

    const NavSpan* span(SpanRef r) const {
        if (r == k_invalid_span) return nullptr;
        const NavTile* t = tile_of(r);
        if (!t) return nullptr;
        uint32_t s = ref_span(r);
        return s < t->spans.size() ? &t->spans[s] : nullptr;
    }

    /** @brief Global cell of a (valid) span. */
    glm::ivec2 cell_of(SpanRef r) const {
        const NavTile& t = *tiles[ref_tile(r)];
        glm::ivec2 l = t.column_of(ref_span(r));
        return {t.tx * params.tile_size + l.x, t.ty * params.tile_size + l.y};
    }

    /** @brief World position of a span: its cell centre at floor height. */
    glm::vec3 position_of(SpanRef r) const {
        glm::ivec2 c = cell_of(r);
        return params.cell_center(c.x, c.y, params.floor_z(span(r)->floor));
    }

    RegionRef region_of(SpanRef r) const { return make_ref(ref_tile(r), span(r)->region); }

    uint32_t component_of(SpanRef r) const {
        const auto& comp = components[ref_tile(r)];
        uint16_t reg = span(r)->region;
        return reg < comp.size() ? comp[reg] : 0xFFFFFFFFu;
    }

    /** @brief The span `r` steps to in direction `d` (0..3), or k_invalid_span. One load: the
     *         neighbour is resolved at build/stitch time (NavSpan::nbr). */
    SpanRef neighbor(SpanRef r, int d) const { return tiles[ref_tile(r)]->spans[ref_span(r)].nbr[d]; }

    /**
     * @brief The diagonal neighbour reached by stepping `da` then `db` (two different axes),
     *        valid only when the other order reaches the same span too -- so a diagonal move
     *        never clips a wall corner.
     */
    SpanRef diagonal(SpanRef r, int da, int db) const {
        SpanRef a = neighbor(r, da);
        if (a == k_invalid_span) return k_invalid_span;
        SpanRef b = neighbor(r, db);
        if (b == k_invalid_span) return k_invalid_span;
        SpanRef x = neighbor(a, db);
        if (x == k_invalid_span) return k_invalid_span;
        return neighbor(b, da) == x ? x : k_invalid_span;
    }

    QueryFilter default_filter() const {
        QueryFilter f;
        f.area_cost = areas.costs;
        return f;
    }

    /** @brief Whether a span may be entered under `filter`. */
    bool passable(const NavSpan& s, const QueryFilter& filter) const {
        if (!filter.passes_area(s.area)) return false;
        if (filter.extra_clearance > 0.0f &&
            static_cast<float>(s.wall_dist) * 0.5f * params.cs < filter.extra_clearance) return false;
        return true;
    }

    /** @brief Traversal cost multiplier of a span under `filter` (area cost x wall penalty). */
    float cost_of(const NavSpan& s, const QueryFilter& filter) const {
        float c = filter.area_cost[s.area];
        if (filter.wall_penalty > 0.0f && filter.wall_penalty_distance > 0.0f) {
            float d = static_cast<float>(s.wall_dist) * 0.5f * params.cs;
            float k = 1.0f - std::min(d / filter.wall_penalty_distance, 1.0f);
            c *= 1.0f + filter.wall_penalty * k;
        }
        return c;
    }

    // --- Spatial queries --------------------------------------------------------------------

    /**
     * @brief The span nearest `pos` within `half_extents`, or k_invalid_span. Distance is 3D
     *        from `pos` to the span's floor point, with a span below `pos` (one the point could
     *        be standing on) preferred over one above it at equal distance.
     */
    SpanRef find_nearest(const glm::vec3& pos, const glm::vec3& half_extents, const QueryFilter* filter = nullptr,
                         glm::vec3* nearest_point = nullptr) const {
        glm::ivec2 c0 = params.cell_of(pos - half_extents);
        glm::ivec2 c1 = params.cell_of(pos + half_extents);
        c0 = glm::max(c0, glm::ivec2(0));
        c1 = glm::min(c1, glm::ivec2(params.cells_x() - 1, params.cells_y() - 1));
        SpanRef best = k_invalid_span;
        float best_d = std::numeric_limits<float>::max();
        glm::vec3 best_p(0.0f);
        int ts = params.tile_size;
        for (int cy = c0.y; cy <= c1.y; ++cy) {
            for (int cx = c0.x; cx <= c1.x; ++cx) {
                const NavTile* t = tile_at(cx / ts, cy / ts);
                if (!t) continue;
                int lx = cx % ts, ly = cy % ts;
                uint32_t first = t->column_first(lx, ly), count = t->column_count(lx, ly);
                for (uint32_t i = first; i < first + count; ++i) {
                    const NavSpan& s = t->spans[i];
                    if (filter && !passable(s, *filter)) continue;
                    float z = params.floor_z(s.floor);
                    if (std::fabs(z - pos.z) > half_extents.z) continue;
                    // Closest point of the cell's floor square to pos -- kept a hair inside the
                    // cell, so a point on a shared edge unambiguously belongs to this span (the
                    // grid raycast starts from the cell the point is in).
                    float x0 = params.origin.x + static_cast<float>(cx) * params.cs;
                    float y0 = params.origin.y + static_cast<float>(cy) * params.cs;
                    const float in = params.cs * 1e-3f;
                    glm::vec3 q(std::clamp(pos.x, x0 + in, x0 + params.cs - in), std::clamp(pos.y, y0 + in, y0 + params.cs - in), z);
                    glm::vec3 dv = q - pos;
                    float d = glm::dot(dv, dv);
                    if (z > pos.z + 0.5f * params.ch) d += 0.25f * params.ch * params.ch; // prefer below
                    if (d < best_d) {
                        best_d = d;
                        best = make_ref(tile_index(t->tx, t->ty), i);
                        best_p = q;
                    }
                }
            }
        }
        if (nearest_point && best != k_invalid_span) *nearest_point = best_p;
        return best;
    }

    /**
     * @brief Fast locate for something already on the surface: the span in `pos`'s own column
     *        that `pos` stands on (highest floor at or below pos.z + climb), falling back to
     *        find_nearest() within one cell and `search_z`.
     */
    SpanRef locate(const glm::vec3& pos, float search_z = 2.0f) const {
        glm::ivec2 c = params.cell_of(pos);
        if (params.cell_in_bounds(c.x, c.y)) {
            int ts = params.tile_size;
            if (const NavTile* t = tile_at(c.x / ts, c.y / ts)) {
                int lx = c.x % ts, ly = c.y % ts;
                uint32_t first = t->column_first(lx, ly), count = t->column_count(lx, ly);
                int qz = params.quantize_z_floor(pos.z) + params.walkable_climb;
                int best = -1;
                for (uint32_t i = first; i < first + count; ++i) {
                    if (static_cast<int>(t->spans[i].floor) <= qz) best = static_cast<int>(i);
                }
                if (best >= 0 && pos.z - params.floor_z(t->spans[static_cast<uint32_t>(best)].floor) <= search_z) {
                    return make_ref(tile_index(t->tx, t->ty), static_cast<uint32_t>(best));
                }
            }
        }
        return find_nearest(pos, glm::vec3(params.cs * 1.5f, params.cs * 1.5f, search_z));
    }

    /**
     * @brief Smooth floor height under `pos` on span `r`: bilinear across the span and its
     *        linked neighbours, so ramps and stairs read as continuous slopes rather than
     *        cell-sized steps. Unlinked neighbours (walls, ledges) fall back to the span itself.
     */
    float floor_height(SpanRef r, const glm::vec3& pos) const {
        const NavSpan* s = span(r);
        if (!s) return pos.z;
        glm::ivec2 c = cell_of(r);
        float fx = (pos.x - params.origin.x) / params.cs - static_cast<float>(c.x) - 0.5f;
        float fy = (pos.y - params.origin.y) / params.cs - static_cast<float>(c.y) - 0.5f;
        int dx = fx >= 0.0f ? 2 : 0;
        int dy = fy >= 0.0f ? 1 : 3;
        float h00 = params.floor_z(s->floor);
        SpanRef nx = neighbor(r, dx), ny = neighbor(r, dy);
        float h10 = nx != k_invalid_span ? params.floor_z(span(nx)->floor) : h00;
        float h01 = ny != k_invalid_span ? params.floor_z(span(ny)->floor) : h00;
        SpanRef nxy = nx != k_invalid_span ? neighbor(nx, dy) : (ny != k_invalid_span ? neighbor(ny, dx) : k_invalid_span);
        float h11 = nxy != k_invalid_span ? params.floor_z(span(nxy)->floor) : (h10 + h01 - h00);
        float ax = std::min(std::fabs(fx), 0.5f), ay = std::min(std::fabs(fy), 0.5f);
        return (h00 * (1.0f - ax) + h10 * ax) * (1.0f - ay) + (h01 * (1.0f - ax) + h11 * ax) * ay;
    }

    /**
     * @brief Walks the surface from `start` (on span `start_ref`) toward `end` in a straight XY
     *        line, following links up and down steps, and stops at the first edge it can't
     *        cross. A clear walk ends on the span in `end`'s column that the line reaches, which
     *        is NOT necessarily the span `end` is on (the line may arrive on a bridge above it):
     *        compare `hit.last` to the target span when that matters.
     * @param max_cost Spans whose cost under `filter` exceeds this block the ray too (path
     *        smoothing uses it so a shortcut never crosses terrain the path deliberately avoided).
     */
    bool raycast(SpanRef start_ref, const glm::vec3& start, const glm::vec3& end, const QueryFilter& filter,
                 NavRaycastHit& hit, float max_cost = std::numeric_limits<float>::max()) const {
        hit = NavRaycastHit{};
        hit.last = start_ref;
        hit.position = start;
        if (!span(start_ref)) {
            hit.blocked = true;
            hit.t = 0.0f;
            return false;
        }
        const float cs = params.cs;
        glm::vec2 o((start.x - params.origin.x) / cs, (start.y - params.origin.y) / cs);
        glm::vec2 e((end.x - params.origin.x) / cs, (end.y - params.origin.y) / cs);
        glm::ivec2 cell = cell_of(start_ref);
        glm::ivec2 target(static_cast<int>(std::floor(e.x)), static_cast<int>(std::floor(e.y)));
        glm::vec2 dvec = e - o;
        int step_x = dvec.x > 0.0f ? 1 : (dvec.x < 0.0f ? -1 : 0);
        int step_y = dvec.y > 0.0f ? 1 : (dvec.y < 0.0f ? -1 : 0);
        const float inf = std::numeric_limits<float>::max();
        float t_dx = step_x != 0 ? std::fabs(1.0f / dvec.x) : inf;
        float t_dy = step_y != 0 ? std::fabs(1.0f / dvec.y) : inf;
        float t_mx = step_x > 0 ? (static_cast<float>(cell.x + 1) - o.x) * t_dx
                   : step_x < 0 ? (o.x - static_cast<float>(cell.x)) * t_dx : inf;
        float t_my = step_y > 0 ? (static_cast<float>(cell.y + 1) - o.y) * t_dy
                   : step_y < 0 ? (o.y - static_cast<float>(cell.y)) * t_dy : inf;
        const int dir_x = step_x > 0 ? 2 : 0;
        const int dir_y = step_y > 0 ? 1 : 3;

        auto enterable = [&](SpanRef n) {
            if (n == k_invalid_span) return false;
            const NavSpan& s = *span(n);
            return passable(s, filter) && cost_of(s, filter) <= max_cost;
        };
        auto block = [&](float t, glm::vec2 normal) {
            hit.blocked = true;
            hit.t = std::clamp(t, 0.0f, 1.0f);
            hit.normal = normal;
            glm::vec3 p = start + (end - start) * hit.t;
            hit.position = glm::vec3(p.x, p.y, params.floor_z(span(hit.last)->floor));
            return false;
        };

        SpanRef cur = start_ref;
        int guard = std::abs(target.x - cell.x) + std::abs(target.y - cell.y) + 4;
        while (cell != target && guard-- > 0) {
            float t;
            const float eps = 1e-6f;
            if (t_mx < t_my - eps) {
                t = t_mx;
                if (t > 1.0f) break;
                SpanRef n = neighbor(cur, dir_x);
                if (!enterable(n)) return block(t, {-static_cast<float>(step_x), 0.0f});
                cur = n;
                cell.x += step_x;
                t_mx += t_dx;
            } else if (t_my < t_mx - eps) {
                t = t_my;
                if (t > 1.0f) break;
                SpanRef n = neighbor(cur, dir_y);
                if (!enterable(n)) return block(t, {0.0f, -static_cast<float>(step_y)});
                cur = n;
                cell.y += step_y;
                t_my += t_dy;
            } else {
                // Exactly through a corner: both L-routes must be clear.
                t = t_mx;
                if (t > 1.0f) break;
                SpanRef a = neighbor(cur, dir_x), b = neighbor(cur, dir_y);
                if (!enterable(a) || !enterable(b)) {
                    return block(t, glm::normalize(glm::vec2(-static_cast<float>(step_x), -static_cast<float>(step_y))));
                }
                SpanRef n = diagonal(cur, dir_x, dir_y);
                if (!enterable(n)) {
                    return block(t, glm::normalize(glm::vec2(-static_cast<float>(step_x), -static_cast<float>(step_y))));
                }
                cur = n;
                cell.x += step_x;
                cell.y += step_y;
                t_mx += t_dx;
                t_my += t_dy;
            }
            hit.last = cur;
        }
        hit.last = cur;
        hit.position = glm::vec3(end.x, end.y, params.floor_z(span(cur)->floor));
        return true;
    }

    /**
     * @brief Moves from `start` toward `target` along the surface, sliding along any edge it
     *        meets (at most three slides), and returns where it ended with a smoothed floor
     *        height. The building block for agent movement.
     */
    NavMoveResult move_along_surface(SpanRef start_ref, const glm::vec3& start, const glm::vec3& target,
                                     const QueryFilter& filter) const {
        NavMoveResult res;
        res.position = start;
        res.ref = start_ref;
        if (!span(start_ref)) return res;
        glm::vec3 from = start;
        glm::vec3 to = target;
        const float eps = params.cs * 1e-3f;
        for (int iter = 0; iter < 3; ++iter) {
            NavRaycastHit hit;
            if (raycast(res.ref, from, to, filter, hit)) {
                res.ref = hit.last;
                res.position = glm::vec3(to.x, to.y, 0.0f);
                break;
            }
            res.blocked = true;
            res.ref = hit.last;
            // Stop just inside the last cell, then slide: drop the blocked axis' component.
            glm::vec2 p = glm::vec2(hit.position) + hit.normal * eps;
            glm::ivec2 c = cell_of(res.ref);
            float x0 = params.origin.x + static_cast<float>(c.x) * params.cs;
            float y0 = params.origin.y + static_cast<float>(c.y) * params.cs;
            p.x = std::clamp(p.x, x0 + eps, x0 + params.cs - eps);
            p.y = std::clamp(p.y, y0 + eps, y0 + params.cs - eps);
            res.position = glm::vec3(p, 0.0f);
            glm::vec2 rem = glm::vec2(to) - p;
            rem -= hit.normal * glm::dot(rem, hit.normal);
            if (glm::dot(rem, rem) < eps * eps) break;
            from = glm::vec3(p, start.z);
            to = glm::vec3(p + rem, target.z);
        }
        res.position.z = floor_height(res.ref, res.position);
        return res;
    }

private:
    /** @brief Re-links the four borders of `t` against the current neighbour tiles, writing
     *         both sides when the neighbour is in this commit's mutable set (it always is: the
     *         neighbour of a replaced tile was copied; a replaced tile's own links are fresh). */
    void stitch_tile_(NavTile& t) {
        const int ts = params.tile_size;
        for (int d = 0; d < 4; ++d) {
            const NavTile* n = tile_at(t.tx + k_dir_dx[d], t.ty + k_dir_dy[d]);
            for (int i = 0; i < ts; ++i) {
                int lx, ly;
                switch (d) {
                    case 0: lx = 0; ly = i; break;
                    case 1: lx = i; ly = ts - 1; break;
                    case 2: lx = ts - 1; ly = i; break;
                    default: lx = i; ly = 0; break;
                }
                int nlx = (lx + k_dir_dx[d] + ts) % ts, nly = (ly + k_dir_dy[d] + ts) % ts;
                uint32_t first = t.column_first(lx, ly), count = t.column_count(lx, ly);
                const uint32_t ni = n ? tile_index(n->tx, n->ty) : 0u;
                for (uint32_t s = first; s < first + count; ++s) {
                    NavSpan& sp = t.spans[s];
                    sp.link[d] = k_no_link;
                    sp.nbr[d] = k_invalid_span;
                    if (!n) continue;
                    uint32_t nf = n->column_first(nlx, nly), nc = n->column_count(nlx, nly);
                    int best = -1, best_dz = 1 << 30;
                    for (uint32_t k = 0; k < nc; ++k) {
                        const NavSpan& o = n->spans[nf + k];
                        if (!detail::spans_connect_(sp.floor, sp.ceiling, o.floor, o.ceiling, params.walkable_height,
                                                    params.walkable_climb)) continue;
                        int dz = std::abs(static_cast<int>(o.floor) - static_cast<int>(sp.floor));
                        if (dz < best_dz) {
                            best_dz = dz;
                            best = static_cast<int>(k);
                        }
                    }
                    if (best >= 0) {
                        sp.link[d] = static_cast<uint8_t>(best);
                        sp.nbr[d] = make_ref(ni, nf + static_cast<uint32_t>(best));
                    }
                }
            }
        }
    }

    /** @brief Rebuilds `t`'s region edges from its four borders. */
    /** @brief Rebuilds `t`'s region edges and portals from its four borders. */
    void build_region_edges_(NavTile& t) {
        const int ts = params.tile_size;
        for (auto& r : t.regions) {
            r.edges.clear();
            r.portals.clear();
        }
        t.portals.clear();
        struct Crossing {
            uint16_t region;
            RegionRef to;
            uint16_t i;
            glm::vec3 pos;
        };
        std::vector<Crossing> crossings;
        for (int d = 0; d < 4; ++d) {
            const NavTile* n = tile_at(t.tx + k_dir_dx[d], t.ty + k_dir_dy[d]);
            if (!n) continue;
            uint32_t ni = tile_index(n->tx, n->ty);
            crossings.clear();
            for (int i = 0; i < ts; ++i) {
                int lx, ly;
                switch (d) {
                    case 0: lx = 0; ly = i; break;
                    case 1: lx = i; ly = ts - 1; break;
                    case 2: lx = ts - 1; ly = i; break;
                    default: lx = i; ly = 0; break;
                }
                int nlx = (lx + k_dir_dx[d] + ts) % ts, nly = (ly + k_dir_dy[d] + ts) % ts;
                uint32_t first = t.column_first(lx, ly), count = t.column_count(lx, ly);
                for (uint32_t s = first; s < first + count; ++s) {
                    const NavSpan& sp = t.spans[s];
                    if (sp.link[d] == k_no_link || sp.link[d] >= n->column_count(nlx, nly)) continue;
                    const NavSpan& o = n->spans[n->column_first(nlx, nly) + sp.link[d]];
                    RegionRef to = make_ref(ni, o.region);
                    crossings.push_back(Crossing{sp.region, to, static_cast<uint16_t>(i),
                                                 params.cell_center(t.tx * ts + lx, t.ty * ts + ly, params.floor_z(sp.floor))});
                    NavRegion& reg = t.regions[sp.region];
                    bool dup = false;
                    for (const auto& e : reg.edges) dup |= e.to == to;
                    if (dup) continue;
                    float cost = glm::distance(reg.centroid, n->regions[o.region].centroid);
                    reg.edges.push_back(RegionEdge{to, cost, -1});
                }
            }
            // Portals: runs of consecutive border cells sharing the same region pair.
            std::sort(crossings.begin(), crossings.end(), [](const Crossing& a, const Crossing& b) {
                if (a.region != b.region) return a.region < b.region;
                if (a.to != b.to) return a.to < b.to;
                return a.i < b.i;
            });
            for (std::size_t k = 0; k < crossings.size();) {
                std::size_t e = k + 1;
                while (e < crossings.size() && crossings[e].region == crossings[k].region && crossings[e].to == crossings[k].to &&
                       crossings[e].i <= crossings[e - 1].i + 1) {
                    ++e;
                }
                if (t.portals.size() < 0xFFFF) {
                    NavPortal p;
                    p.dir = static_cast<uint8_t>(d);
                    p.region = crossings[k].region;
                    p.to = crossings[k].to;
                    p.first = crossings[k].i;
                    p.count = static_cast<uint16_t>(crossings[e - 1].i - crossings[k].i + 1);
                    p.mid = crossings[k + (e - k) / 2].pos;
                    t.regions[p.region].portals.push_back(static_cast<uint16_t>(t.portals.size()));
                    t.portals.push_back(p);
                }
                k = e;
            }
        }
    }

public:
    /**
     * @brief The portal in the neighbour tile that mirrors portal `p` of tile `tile` (the same
     *        border cells seen from the other side), or 0xFFFF. A linear scan of the neighbour's
     *        portals: a tile has a handful.
     */
    uint16_t mirror_portal(uint32_t tile, uint16_t p) const {
        const NavTile* t = tiles[tile].get();
        if (!t || p >= t->portals.size()) return 0xFFFF;
        const NavPortal& a = t->portals[p];
        const NavTile* n = tiles[ref_tile(a.to)].get();
        if (!n) return 0xFFFF;
        const RegionRef back = make_ref(tile, a.region);
        const int od = opposite_dir(a.dir);
        uint16_t best = 0xFFFF;
        int best_overlap = 0;
        for (uint16_t q = 0; q < n->portals.size(); ++q) {
            const NavPortal& b = n->portals[q];
            if (b.dir != od || b.region != ref_span(a.to) || b.to != back) continue;
            int lo = std::max<int>(a.first, b.first), hi = std::min<int>(a.first + a.count, b.first + b.count);
            if (hi - lo > best_overlap) {
                best_overlap = hi - lo;
                best = q;
            }
        }
        return best;
    }

private:
    void resolve_links_(const std::vector<OffMeshLink>& link_set) {
        links = link_set;
        link_edges.clear();
        link_region_edges.clear();
        has_one_way_links = false;
        for (std::size_t i = 0; i < links.size() && i < 0x7FFF; ++i) {
            OffMeshLink& l = links[i];
            glm::vec3 ext(l.snap_radius, l.snap_radius, std::max(l.snap_radius, params.agent_height));
            l.start_ref = find_nearest(l.start, ext);
            l.end_ref = find_nearest(l.end, ext);
            if (l.start_ref == k_invalid_span || l.end_ref == k_invalid_span || l.start_ref == l.end_ref) continue;
            float len = glm::distance(l.start, l.end) * std::max(l.cost, 0.0f);
            auto add = [&](SpanRef a, SpanRef b, bool reverse) {
                link_edges[a].push_back(LinkEdge{static_cast<uint16_t>(i), b, reverse});
                link_region_edges[region_of(a)].push_back(RegionEdge{region_of(b), len, static_cast<int16_t>(i)});
            };
            add(l.start_ref, l.end_ref, false);
            if (l.bidirectional) add(l.end_ref, l.start_ref, true);
            else has_one_way_links = true;
        }
    }

    /** @brief Union-find over every region, joined by border edges and off-mesh links. */
    void build_components_() {
        std::vector<uint32_t> base(tiles.size() + 1, 0);
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            base[i + 1] = base[i] + (tiles[i] ? static_cast<uint32_t>(tiles[i]->regions.size()) : 0u);
        }
        std::vector<uint32_t> parent(base.back());
        std::iota(parent.begin(), parent.end(), 0u);
        auto find = [&](uint32_t x) {
            while (parent[x] != x) {
                parent[x] = parent[parent[x]];
                x = parent[x];
            }
            return x;
        };
        auto unite = [&](uint32_t a, uint32_t b) {
            a = find(a);
            b = find(b);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        };
        auto flat = [&](RegionRef r) { return base[ref_tile(r)] + ref_span(r); };
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (!tiles[i]) continue;
            for (std::size_t r = 0; r < tiles[i]->regions.size(); ++r) {
                for (const auto& e : tiles[i]->regions[r].edges) {
                    if (ref_tile(e.to) < tiles.size() && tiles[ref_tile(e.to)] &&
                        ref_span(e.to) < tiles[ref_tile(e.to)]->regions.size()) {
                        unite(base[i] + static_cast<uint32_t>(r), flat(e.to));
                    }
                }
            }
        }
        // Off-mesh links join components only if bidirectional: a one-way drop makes the lower
        // area reachable from the upper one, not the other way round, and components are
        // symmetric. One-way links therefore keep their components separate and path queries
        // fall back to searching (see path_query.h's reachability check).
        for (const auto& l : links) {
            if (l.bidirectional && l.start_ref != k_invalid_span && l.end_ref != k_invalid_span) {
                unite(flat(region_of(l.start_ref)), flat(region_of(l.end_ref)));
            }
        }
        components.assign(tiles.size(), {});
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (!tiles[i]) continue;
            components[i].resize(tiles[i]->regions.size());
            for (std::size_t r = 0; r < tiles[i]->regions.size(); ++r) {
                components[i][r] = find(base[i] + static_cast<uint32_t>(r));
            }
        }
    }
};

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_MESH_H
