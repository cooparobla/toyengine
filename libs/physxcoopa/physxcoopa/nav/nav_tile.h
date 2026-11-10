/**
 * @file nav_tile.h
 * @brief NavTile -- one tile of the walkable surface for one agent profile -- and the pipeline
 *        that builds it from a padded solid Heightfield.
 *
 * Build steps, all on the padded (tile + border) grid so erosion near a tile edge sees the
 * geometry beyond it and two neighbouring tiles agree on their shared edge:
 *   1. open spans: the free space above every walkable solid span, kept when its clearance is
 *      at least the agent height; NavVolumes relabel areas (or cut holes).
 *   2. links: each open span links to the span in each 4-neighbour column it can step to
 *      (floor difference <= climb, shared clearance >= height).
 *   3. erosion: a two-pass chamfer distance field from the unlinked boundary; spans closer to
 *      a wall than the agent radius are dropped, and the remaining distance is kept per span
 *      as `wall_dist` (flow-field and wall-penalty costs read it).
 *   4. crop to the tile interior, remapping links; links that leave the tile are resolved
 *      later, against the neighbour tile, by NavMesh's stitch step.
 *   5. regions: tile-local connected components, the nodes of the coarse graph.
 */

#ifndef PHYSXCOOPA_NAV_NAV_TILE_H
#define PHYSXCOOPA_NAV_NAV_TILE_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_source.h>
#include <physxcoopa/nav/heightfield.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/**
 * @struct NavSpan
 * @brief One walkable floor in one column (32 bytes).
 */
struct NavSpan {
    uint16_t column = 0;                /**< Tile-local column, lx + ly * tile_size. */
    uint16_t floor = 0;                 /**< Floor height, z quanta. */
    uint16_t ceiling = k_open_ceiling;  /**< Underside of whatever is above, or k_open_ceiling. */
    uint16_t region = 0;                /**< Tile-local region index. */
    uint8_t area = k_area_walkable;
    uint8_t wall_dist = 0;              /**< Distance to the nearest edge, half-cells (capped). */
    uint8_t link[4] = {k_no_link, k_no_link, k_no_link, k_no_link}; /**< Layer index in each neighbour column. */
    /** @brief The linked neighbour span in each direction, fully resolved (k_invalid_span if
     *         none) -- so NavMesh::neighbor() is one load, not a tile/column walk. Set by
     *         build_tile() inside the tile and by NavMesh's stitch step across tile borders. */
    SpanRef nbr[4] = {k_invalid_span, k_invalid_span, k_invalid_span, k_invalid_span};
};

/** @brief A coarse-graph edge from one region to another. */
struct RegionEdge {
    RegionRef to = k_invalid_region;
    float cost = 0.0f;
    int16_t link = -1;   /**< Off-mesh link index, or -1 for an ordinary tile-border crossing. */
};

/** @brief A tile-local connected patch of spans. */
struct NavRegion {
    glm::vec3 centroid{0.0f};
    uint32_t span_count = 0;
    std::vector<RegionEdge> edges;
    std::vector<uint16_t> portals;   /**< Indices into NavTile::portals on this region's border. */
};

/**
 * @struct NavPortal
 * @brief A contiguous run of walkable crossings over one tile border, between one region of
 *        this tile and one region of the neighbour. The node type of the portal graph that
 *        hierarchical flow fields route over (flow_field.h). The neighbour tile holds the
 *        mirrored portal (same border cells, seen from its side); it is looked up by
 *        NavMesh::mirror_portal() rather than stored, so a neighbour's rebuild never leaves a
 *        stale index here.
 */
struct NavPortal {
    uint8_t dir = 0;                 /**< Border side (k_dir_dx/dy order). */
    uint16_t region = 0;             /**< This tile's region. */
    RegionRef to = k_invalid_region; /**< The neighbour (tile, region) it opens onto. */
    uint16_t first = 0;              /**< First border cell index along the side. */
    uint16_t count = 0;              /**< Border cells it spans. */
    glm::vec3 mid{0.0f};             /**< Midpoint, on this tile's side, at floor height. */
};

/**
 * @struct NavTile
 * @brief Immutable once published (NavMesh hands tiles out as shared_ptr<const NavTile>).
 */
struct NavTile {
    int tx = 0;
    int ty = 0;
    uint32_t revision = 0;               /**< Globally unique per build; flow fields key on it. */
    int size = 0;                        /**< Cells per side (NavGridParams::tile_size). */
    /** @brief Per column (lx + ly * size): `(first_span << 8) | count`. */
    std::vector<uint32_t> columns;
    std::vector<NavSpan> spans;          /**< Grouped by column, floor-ascending within one. */
    std::vector<NavRegion> regions;
    std::vector<NavPortal> portals;      /**< Built with the region edges by NavMesh::commit(). */

    uint32_t column_first(int lx, int ly) const { return columns[static_cast<std::size_t>(lx + ly * size)] >> 8; }
    uint32_t column_count(int lx, int ly) const { return columns[static_cast<std::size_t>(lx + ly * size)] & 0xFFu; }
    /** @brief Tile-local column (lx, ly) of the span at index `s`. */
    glm::ivec2 column_of(uint32_t s) const {
        uint32_t c = spans[s].column;
        return {static_cast<int>(c % static_cast<uint32_t>(size)), static_cast<int>(c / static_cast<uint32_t>(size))};
    }
};

/** @brief Reusable per-thread working memory for build_tile(). */
struct TileBuildScratch {
    struct PSpan {
        uint16_t floor = 0;
        uint16_t ceiling = k_open_ceiling;
        uint8_t area = k_area_null;
        uint8_t dist = 0;
        uint8_t link[4] = {k_no_link, k_no_link, k_no_link, k_no_link};
    };
    std::vector<uint32_t> pcols;      /**< Padded columns, (first << 8) | count. */
    std::vector<PSpan> pspans;
    std::vector<uint8_t> areas;       /**< Heightfield areas after low-hanging filtering. */
    std::vector<uint16_t> remap;      /**< Padded span -> new layer index in its column (0xFFFF dropped). */
    std::vector<uint32_t> stack;
};

namespace detail {

/** @brief Whether open spans (f0, c0) and (f1, c1) can be stepped between. */
inline bool spans_connect_(uint16_t f0, uint16_t c0, uint16_t f1, uint16_t c1, int walkable_height, int climb) {
    int bot = std::max<int>(f0, f1);
    int top = std::min<int>(c0, c1);
    return top - bot >= walkable_height && std::abs(static_cast<int>(f0) - static_cast<int>(f1)) <= climb;
}

} // namespace detail

/**
 * @brief Builds tile (tx, ty) from `hf`, a heightfield covering the tile plus
 *        `params.border` cells on every side, already rasterized.
 *
 * @param volumes   Area volumes overlapping the padded tile.
 * @param revision  Stored on the tile (NavBaker hands out a fresh value per build).
 */
inline void build_tile(const Heightfield& hf, const NavGridParams& params, int tx, int ty,
                       const std::vector<SourceVolume>& volumes, uint32_t revision, NavTile& out,
                       TileBuildScratch& scratch) {
    const int b = params.border;
    const int ts = params.tile_size;
    const int W = hf.width();
    const int H = hf.height();
    const int wh = params.walkable_height;
    const int climb = params.walkable_climb;

    hf.filter_low_hanging_obstacles(climb, scratch.areas);

    // 1. Open spans.
    auto& pcols = scratch.pcols;
    auto& ps = scratch.pspans;
    pcols.assign(static_cast<std::size_t>(W) * H, 0u);
    ps.clear();
    const float px0 = params.origin.x + static_cast<float>(tx * ts - b) * params.cs;
    const float py0 = params.origin.y + static_cast<float>(ty * ts - b) * params.cs;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t first = static_cast<uint32_t>(ps.size());
            uint32_t count = 0;
            for (uint32_t s = hf.head(x, y); s != Heightfield::k_null; s = hf.span(s).next) {
                uint8_t area = scratch.areas[s];
                if (area == k_area_null) continue;
                const SolidSpan& sp = hf.span(s);
                uint16_t floor = sp.smax;
                uint16_t ceiling = sp.next != Heightfield::k_null ? hf.span(sp.next).smin : k_open_ceiling;
                if (ceiling != k_open_ceiling && static_cast<int>(ceiling) - static_cast<int>(floor) < wh) continue;
                if (!volumes.empty()) {
                    glm::vec3 p(px0 + (static_cast<float>(x) + 0.5f) * params.cs,
                                py0 + (static_cast<float>(y) + 0.5f) * params.cs,
                                params.floor_z(floor) + 0.5f * params.ch);
                    for (const SourceVolume& v : volumes) {
                        if (v.contains(p)) area = v.area;
                    }
                    if (area == k_area_null) continue;
                }
                if (count >= 254) break; // layer indices are uint8 with 0xFF reserved
                TileBuildScratch::PSpan o;
                o.floor = floor;
                o.ceiling = ceiling;
                o.area = area;
                ps.push_back(o);
                ++count;
            }
            pcols[static_cast<std::size_t>(x + y * W)] = (first << 8) | count;
        }
    }

    // 2. Links.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t c = pcols[static_cast<std::size_t>(x + y * W)];
            for (uint32_t i = c >> 8, e = (c >> 8) + (c & 0xFFu); i < e; ++i) {
                TileBuildScratch::PSpan& s = ps[i];
                for (int d = 0; d < 4; ++d) {
                    int nx = x + k_dir_dx[d], ny = y + k_dir_dy[d];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    uint32_t nc = pcols[static_cast<std::size_t>(nx + ny * W)];
                    int best = -1, best_dz = 1 << 30;
                    for (uint32_t k = 0; k < (nc & 0xFFu); ++k) {
                        const TileBuildScratch::PSpan& n = ps[(nc >> 8) + k];
                        if (!detail::spans_connect_(s.floor, s.ceiling, n.floor, n.ceiling, wh, climb)) continue;
                        int dz = std::abs(static_cast<int>(n.floor) - static_cast<int>(s.floor));
                        if (dz < best_dz) {
                            best_dz = dz;
                            best = static_cast<int>(k);
                        }
                    }
                    s.link[d] = best >= 0 ? static_cast<uint8_t>(best) : k_no_link;
                }
            }
        }
    }

    // 3. Distance field (chamfer 2/3, half-cell units) and erosion.
    auto nbr = [&](int x, int y, const TileBuildScratch::PSpan& s, int d) -> TileBuildScratch::PSpan* {
        if (s.link[d] == k_no_link) return nullptr;
        int nx = x + k_dir_dx[d], ny = y + k_dir_dy[d];
        return &ps[(pcols[static_cast<std::size_t>(nx + ny * W)] >> 8) + s.link[d]];
    };
    for (auto& s : ps) {
        bool boundary = false;
        for (int d = 0; d < 4; ++d) boundary |= s.link[d] == k_no_link;
        s.dist = boundary ? 0 : 255;
    }
    auto relax = [](uint8_t& d, int v) { if (v < d) d = static_cast<uint8_t>(std::min(v, 255)); };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t c = pcols[static_cast<std::size_t>(x + y * W)];
            for (uint32_t i = c >> 8, e = (c >> 8) + (c & 0xFFu); i < e; ++i) {
                TileBuildScratch::PSpan& s = ps[i];
                if (auto* a = nbr(x, y, s, 0)) {                 // (-1, 0)
                    relax(s.dist, a->dist + 2);
                    if (auto* aa = nbr(x - 1, y, *a, 3)) relax(s.dist, aa->dist + 3); // (-1, -1)
                }
                if (auto* a = nbr(x, y, s, 3)) {                 // (0, -1)
                    relax(s.dist, a->dist + 2);
                    if (auto* aa = nbr(x, y - 1, *a, 2)) relax(s.dist, aa->dist + 3); // (+1, -1)
                }
            }
        }
    }
    for (int y = H - 1; y >= 0; --y) {
        for (int x = W - 1; x >= 0; --x) {
            uint32_t c = pcols[static_cast<std::size_t>(x + y * W)];
            for (uint32_t i = c >> 8, e = (c >> 8) + (c & 0xFFu); i < e; ++i) {
                TileBuildScratch::PSpan& s = ps[i];
                if (auto* a = nbr(x, y, s, 2)) {                 // (+1, 0)
                    relax(s.dist, a->dist + 2);
                    if (auto* aa = nbr(x + 1, y, *a, 1)) relax(s.dist, aa->dist + 3); // (+1, +1)
                }
                if (auto* a = nbr(x, y, s, 1)) {                 // (0, +1)
                    relax(s.dist, a->dist + 2);
                    if (auto* aa = nbr(x, y + 1, *a, 0)) relax(s.dist, aa->dist + 3); // (-1, +1)
                }
            }
        }
    }
    const int erode = params.walkable_radius * 2;
    for (auto& s : ps) {
        if (s.dist < erode) s.area = k_area_null;
        else s.dist = static_cast<uint8_t>(s.dist - erode);
    }

    // 4. Crop to the interior and remap links.
    const uint32_t tile_index = static_cast<uint32_t>(tx + ty * params.tiles_x);
    out.tx = tx;
    out.ty = ty;
    out.size = ts;
    out.revision = revision;
    out.columns.assign(static_cast<std::size_t>(ts) * ts, 0u);
    out.spans.clear();
    out.regions.clear();
    auto& remap = scratch.remap;
    remap.assign(ps.size(), 0xFFFFu);
    for (int ly = 0; ly < ts; ++ly) {
        for (int lx = 0; lx < ts; ++lx) {
            uint32_t c = pcols[static_cast<std::size_t>((lx + b) + (ly + b) * W)];
            uint32_t first = static_cast<uint32_t>(out.spans.size());
            uint32_t n = 0;
            for (uint32_t i = c >> 8, e = (c >> 8) + (c & 0xFFu); i < e; ++i) {
                if (ps[i].area == k_area_null) continue;
                if (out.spans.size() >= 0xFFFFu) break; // SpanRef holds 16 bits of span index
                remap[i] = static_cast<uint16_t>(n++);
                NavSpan o;
                o.column = static_cast<uint16_t>(lx + ly * ts);
                o.floor = ps[i].floor;
                o.ceiling = ps[i].ceiling;
                o.area = ps[i].area;
                o.wall_dist = ps[i].dist;
                out.spans.push_back(o);
            }
            out.columns[static_cast<std::size_t>(lx + ly * ts)] = (first << 8) | n;
        }
    }
    for (int ly = 0; ly < ts; ++ly) {
        for (int lx = 0; lx < ts; ++lx) {
            int x = lx + b, y = ly + b;
            uint32_t c = pcols[static_cast<std::size_t>(x + y * W)];
            for (uint32_t i = c >> 8, e = (c >> 8) + (c & 0xFFu); i < e; ++i) {
                if (remap[i] == 0xFFFFu) continue;
                NavSpan& o = out.spans[out.column_first(lx, ly) + remap[i]];
                for (int d = 0; d < 4; ++d) {
                    int nlx = lx + k_dir_dx[d], nly = ly + k_dir_dy[d];
                    if (nlx < 0 || nly < 0 || nlx >= ts || nly >= ts) continue; // stitched later
                    if (ps[i].link[d] == k_no_link) continue;
                    uint32_t nc = pcols[static_cast<std::size_t>((x + k_dir_dx[d]) + (y + k_dir_dy[d]) * W)];
                    uint16_t r = remap[(nc >> 8) + ps[i].link[d]];
                    if (r != 0xFFFFu) {
                        o.link[d] = static_cast<uint8_t>(r);
                        o.nbr[d] = make_ref(tile_index, out.column_first(nlx, nly) + r);
                    }
                }
            }
        }
    }

    // 5. Regions: flood fill over intra-tile links.
    constexpr uint16_t k_unassigned = 0xFFFFu;
    for (auto& s : out.spans) s.region = k_unassigned;
    auto& stack = scratch.stack;
    for (int ly = 0; ly < ts; ++ly) {
        for (int lx = 0; lx < ts; ++lx) {
            uint32_t first = out.column_first(lx, ly), count = out.column_count(lx, ly);
            for (uint32_t i = first; i < first + count; ++i) {
                if (out.spans[i].region != k_unassigned) continue;
                if (out.regions.size() >= 0xFFFEu) break;
                uint16_t rid = static_cast<uint16_t>(out.regions.size());
                out.regions.emplace_back();
                NavRegion& reg = out.regions.back();
                glm::dvec3 sum(0.0);
                stack.clear();
                // Pack (span index, column) so the fill never has to search for a span's column.
                stack.push_back(i);
                stack.push_back(static_cast<uint32_t>(lx + ly * ts));
                out.spans[i].region = rid;
                while (!stack.empty()) {
                    uint32_t col = stack.back(); stack.pop_back();
                    uint32_t si = stack.back(); stack.pop_back();
                    int cx = static_cast<int>(col % static_cast<uint32_t>(ts)), cy = static_cast<int>(col / static_cast<uint32_t>(ts));
                    const NavSpan& s = out.spans[si];
                    sum += glm::dvec3(params.cell_center(tx * ts + cx, ty * ts + cy, params.floor_z(s.floor)));
                    ++reg.span_count;
                    for (int d = 0; d < 4; ++d) {
                        if (s.link[d] == k_no_link) continue;
                        int nx = cx + k_dir_dx[d], ny = cy + k_dir_dy[d];
                        if (nx < 0 || ny < 0 || nx >= ts || ny >= ts) continue;
                        uint32_t ni = out.column_first(nx, ny) + s.link[d];
                        if (out.spans[ni].region != k_unassigned) continue;
                        out.spans[ni].region = rid;
                        stack.push_back(ni);
                        stack.push_back(static_cast<uint32_t>(nx + ny * ts));
                    }
                }
                reg.centroid = glm::vec3(sum / static_cast<double>(reg.span_count));
            }
        }
    }
    for (auto& s : out.spans) {
        if (s.region == k_unassigned) s.region = 0; // only reachable if the region cap was hit
    }
}

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_TILE_H
