/**
 * @file path_query.h
 * @brief A* path queries over a NavMesh: hierarchical (coarse region corridor, then fine span
 *        search), reachability-aware, smoothed, and batchable across the job engine.
 *
 * One query, find_path():
 *   1. Snap start and goal to spans (NavMesh::find_nearest).
 *   2. Reachability: different connected components means no complete path exists, so the
 *      query either fails immediately (allow_partial == false) or searches for the closest
 *      reachable point within the node budget -- instead of flooding the whole world first.
 *   3. Corridor: for goals more than one tile away, A* over the coarse region graph picks the
 *      chain of regions the path passes through; the fine search is then confined to that
 *      corridor plus a one-region margin, which is what keeps long queries cheap. If the
 *      confined search fails (a filter blocked a gap the coarse graph thought open) it is retried
 *      unconfined.
 *   4. Fine A*: 8-connected over spans (diagonals never cut corners), plus off-mesh links;
 *      cost = 3D step length x the destination's cost under the filter; octile/3D heuristic.
 *   5. Smoothing: greedy string pulling with NavMesh::raycast(), which walks the surface and
 *      follows steps, so shortcuts are taken across floors and up stairs but never through
 *      walls or across terrain that costs more than what the raw path crossed.
 *
 * All scratch memory lives in a PathQueryScratch (one per thread -- see thread_scratch()) whose
 * node pool is cleared in O(1) by a generation stamp, so a query allocates nothing once warm.
 */

#ifndef PHYSXCOOPA_NAV_PATH_QUERY_H
#define PHYSXCOOPA_NAV_PATH_QUERY_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_mesh.h>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/**
 * @class StampedMap
 * @brief Open-addressing uint32 -> uint32 map cleared in O(1): an entry is live only if its
 *        stamp matches the current generation.
 */
class StampedMap {
public:
    static constexpr uint32_t k_none = 0xFFFFFFFFu;

    void clear() {
        if (++stamp_ == 0) { // wrapped: really clear once every 4 billion queries
            for (auto& e : entries_) e.stamp = 0;
            stamp_ = 1;
        }
        size_ = 0;
    }

    uint32_t find(uint32_t key) const {
        if (entries_.empty()) return k_none;
        std::size_t mask = entries_.size() - 1;
        for (std::size_t i = hash_(key) & mask;; i = (i + 1) & mask) {
            const Entry& e = entries_[i];
            if (e.stamp != stamp_) return k_none;
            if (e.key == key) return e.value;
        }
    }

    /** @brief Inserts or overwrites. */
    void set(uint32_t key, uint32_t value) {
        if ((size_ + 1) * 2 > entries_.size()) grow_();
        std::size_t mask = entries_.size() - 1;
        for (std::size_t i = hash_(key) & mask;; i = (i + 1) & mask) {
            Entry& e = entries_[i];
            if (e.stamp != stamp_) {
                e = Entry{key, value, stamp_};
                ++size_;
                return;
            }
            if (e.key == key) {
                e.value = value;
                return;
            }
        }
    }

    std::size_t size() const { return size_; }

private:
    struct Entry {
        uint32_t key = 0;
        uint32_t value = 0;
        uint32_t stamp = 0;
    };

    static std::size_t hash_(uint32_t k) {
        k ^= k >> 16;
        k *= 0x7feb352du;
        k ^= k >> 15;
        k *= 0x846ca68bu;
        k ^= k >> 16;
        return k;
    }

    void grow_() {
        std::vector<Entry> old;
        old.swap(entries_);
        uint32_t old_stamp = stamp_;
        entries_.assign(std::max<std::size_t>(64, old.size() * 2), Entry{});
        stamp_ = 1;
        size_ = 0;
        for (const Entry& e : old) {
            if (e.stamp == old_stamp) set(e.key, e.value);
        }
    }

    std::vector<Entry> entries_;
    uint32_t stamp_ = 1;
    std::size_t size_ = 0;
};

/** @brief One A* node (fine or coarse). */
struct SearchNode {
    uint32_t ref = 0;        /**< SpanRef or RegionRef. */
    uint32_t parent = StampedMap::k_none;
    float g = 0.0f;
    float f = 0.0f;
    uint32_t heap_index = StampedMap::k_none;
    int16_t via_link = -1;   /**< Off-mesh link used to reach this node. */
    bool closed = false;
};

/**
 * @class SearchSpace
 * @brief Node storage + indexed binary min-heap on f (ties broken toward larger g, which
 *        expands fewer nodes on open ground).
 */
class SearchSpace {
public:
    void reset() {
        index_.clear();
        nodes_.clear();
        heap_.clear();
    }

    /** @brief Node index for `ref`, creating it when absent. */
    uint32_t node(uint32_t ref, bool& created) {
        uint32_t i = index_.find(ref);
        created = i == StampedMap::k_none;
        if (created) {
            i = static_cast<uint32_t>(nodes_.size());
            SearchNode n;
            n.ref = ref;
            nodes_.push_back(n);
            index_.set(ref, i);
        }
        return i;
    }

    SearchNode& at(uint32_t i) { return nodes_[i]; }
    const SearchNode& at(uint32_t i) const { return nodes_[i]; }
    std::size_t node_count() const { return nodes_.size(); }

    bool heap_empty() const { return heap_.empty(); }

    void push_or_update(uint32_t i) {
        SearchNode& n = nodes_[i];
        if (n.heap_index == StampedMap::k_none) {
            n.heap_index = static_cast<uint32_t>(heap_.size());
            heap_.push_back(i);
        }
        sift_up_(n.heap_index);
    }

    uint32_t pop() {
        uint32_t top = heap_[0];
        nodes_[top].heap_index = StampedMap::k_none;
        uint32_t last = heap_.back();
        heap_.pop_back();
        if (!heap_.empty()) {
            heap_[0] = last;
            nodes_[last].heap_index = 0;
            sift_down_(0);
        }
        return top;
    }

private:
    bool less_(uint32_t a, uint32_t b) const {
        const SearchNode& na = nodes_[a];
        const SearchNode& nb = nodes_[b];
        if (na.f != nb.f) return na.f < nb.f;
        return na.g > nb.g;
    }
    void sift_up_(uint32_t pos) {
        uint32_t item = heap_[pos];
        while (pos > 0) {
            uint32_t parent = (pos - 1) / 2;
            if (!less_(item, heap_[parent])) break;
            heap_[pos] = heap_[parent];
            nodes_[heap_[pos]].heap_index = pos;
            pos = parent;
        }
        heap_[pos] = item;
        nodes_[item].heap_index = pos;
    }
    void sift_down_(uint32_t pos) {
        uint32_t item = heap_[pos];
        uint32_t n = static_cast<uint32_t>(heap_.size());
        for (;;) {
            uint32_t child = pos * 2 + 1;
            if (child >= n) break;
            if (child + 1 < n && less_(heap_[child + 1], heap_[child])) ++child;
            if (!less_(heap_[child], item)) break;
            heap_[pos] = heap_[child];
            nodes_[heap_[pos]].heap_index = pos;
            pos = child;
        }
        heap_[pos] = item;
        nodes_[item].heap_index = pos;
    }

    StampedMap index_;
    std::vector<SearchNode> nodes_;
    std::vector<uint32_t> heap_;
};

/** @brief Reusable per-thread memory for path queries. */
struct PathQueryScratch {
    SearchSpace fine;
    SearchSpace coarse;
    StampedMap corridor;          /**< RegionRef -> 1 for regions the fine search may enter. */
    std::vector<SpanRef> refs;
    std::vector<int16_t> via;
    std::vector<RegionRef> regions;
};

/** @brief The calling thread's PathQueryScratch. */
inline PathQueryScratch& thread_scratch() {
    thread_local PathQueryScratch s;
    return s;
}

/** @brief Tunables of a path query that are not traversal rules (those are QueryFilter). */
struct PathQueryOptions {
    /** @brief How far start/goal may be from the surface and still snap to it. */
    glm::vec3 search_extents{2.0f, 2.0f, 4.0f};
    /** @brief Confine long searches to the coarse corridor (see the file doc). */
    bool use_corridor = true;
    /** @brief Regions around the coarse corridor the fine search may also use. 0 or 1. */
    int corridor_margin = 1;
    /** @brief String-pull the result. */
    bool smooth = true;
    /** @brief Longest single smoothing look-ahead, in path steps. Bounds smoothing cost. */
    int max_smoothing_lookahead = 96;
};

namespace detail {

inline float octile_3d_(const glm::vec3& a, const glm::vec3& b) {
    float dx = std::fabs(a.x - b.x), dy = std::fabs(a.y - b.y), dz = a.z - b.z;
    float o = std::max(dx, dy) + 0.41421356f * std::min(dx, dy);
    return std::sqrt(o * o + dz * dz);
}

inline float min_area_cost_(const NavMesh& mesh, const QueryFilter& f) {
    float c = std::numeric_limits<float>::max();
    for (uint32_t a = 1; a < k_max_areas; ++a) {
        if ((f.area_mask >> a) & 1ull) c = std::min(c, f.area_cost[a]);
    }
    (void)mesh;
    return c == std::numeric_limits<float>::max() ? 1.0f : std::max(c, 0.0f);
}

/** @brief Coarse A* from region `from` to `to`, filling scratch.corridor. */
inline bool plan_corridor_(const NavMesh& mesh, RegionRef from, RegionRef to, float min_cost, int margin,
                           PathQueryScratch& scratch) {
    SearchSpace& sp = scratch.coarse;
    sp.reset();
    auto centroid = [&](RegionRef r) { return mesh.tiles[ref_tile(r)]->regions[ref_span(r)].centroid; };
    auto valid_region = [&](RegionRef r) {
        uint32_t t = ref_tile(r);
        return t < mesh.tiles.size() && mesh.tiles[t] && ref_span(r) < mesh.tiles[t]->regions.size();
    };
    glm::vec3 goal_c = centroid(to);
    bool created;
    uint32_t s = sp.node(from, created);
    sp.at(s).g = 0.0f;
    sp.at(s).f = glm::distance(centroid(from), goal_c) * min_cost;
    sp.push_or_update(s);
    uint32_t found = StampedMap::k_none;
    auto relax = [&](uint32_t cur, const RegionEdge& e) {
        if (!valid_region(e.to)) return;
        uint32_t n = sp.node(e.to, created);
        SearchNode& nn = sp.at(n);
        if (nn.closed) return;
        float g = sp.at(cur).g + e.cost * min_cost;
        if (!created && g >= nn.g) return;
        nn.g = g;
        nn.f = g + glm::distance(centroid(e.to), goal_c) * min_cost;
        nn.parent = cur;
        sp.push_or_update(n);
    };
    while (!sp.heap_empty()) {
        uint32_t cur = sp.pop();
        sp.at(cur).closed = true;
        RegionRef r = sp.at(cur).ref;
        if (r == to) {
            found = cur;
            break;
        }
        const NavRegion& reg = mesh.tiles[ref_tile(r)]->regions[ref_span(r)];
        for (const RegionEdge& e : reg.edges) relax(cur, e);
        auto it = mesh.link_region_edges.find(r);
        if (it != mesh.link_region_edges.end()) {
            for (const RegionEdge& e : it->second) relax(cur, e);
        }
    }
    if (found == StampedMap::k_none) return false;

    scratch.corridor.clear();
    scratch.regions.clear();
    for (uint32_t n = found; n != StampedMap::k_none; n = sp.at(n).parent) scratch.regions.push_back(sp.at(n).ref);
    for (RegionRef r : scratch.regions) {
        scratch.corridor.set(r, 1);
        if (margin <= 0) continue;
        for (const RegionEdge& e : mesh.tiles[ref_tile(r)]->regions[ref_span(r)].edges) scratch.corridor.set(e.to, 1);
    }
    return true;
}

/** @brief Fine A*. Returns the goal node, or (partial) the node closest to the goal. */
inline uint32_t search_(const NavMesh& mesh, SpanRef start, SpanRef goal, const glm::vec3& goal_pos,
                        const QueryFilter& filter, float min_cost, const StampedMap* corridor, PathQueryScratch& scratch,
                        bool& reached, uint32_t& expanded) {
    SearchSpace& sp = scratch.fine;
    sp.reset();
    reached = false;
    expanded = 0;
    const float hw = std::max(filter.heuristic_weight, 0.0f) * min_cost;
    bool created;
    uint32_t s = sp.node(start, created);
    glm::vec3 sp_pos = mesh.position_of(start);
    sp.at(s).g = 0.0f;
    sp.at(s).f = detail::octile_3d_(sp_pos, goal_pos) * hw;
    sp.push_or_update(s);
    uint32_t best = s;
    float best_h = sp.at(s).f;

    auto try_edge = [&](uint32_t cur, SpanRef to, const glm::vec3& to_pos, float step_cost, int16_t via_link) {
        uint32_t n = sp.node(to, created);
        SearchNode& nn = sp.at(n);
        if (nn.closed) return;
        float g = sp.at(cur).g + step_cost;
        if (!created && g >= nn.g) return;
        float h = detail::octile_3d_(to_pos, goal_pos) * hw;
        nn.g = g;
        nn.f = g + h;
        nn.parent = cur;
        nn.via_link = via_link;
        sp.push_or_update(n);
        if (h < best_h) {
            best_h = h;
            best = n;
        }
    };
    auto enterable = [&](SpanRef r) {
        if (r == k_invalid_span) return false;
        if (!mesh.passable(*mesh.span(r), filter)) return false;
        if (corridor && corridor->find(mesh.region_of(r)) == StampedMap::k_none) return false;
        return true;
    };
    const float cs = mesh.params.cs;
    const float ch = mesh.params.ch;
    // Neighbour positions are the current one plus a cell step and the floor difference --
    // cheaper than resolving every neighbour's cell through its tile.
    auto step_to = [&](const glm::vec3& from, uint16_t from_floor, SpanRef to, int dx, int dy, float& len) {
        const NavSpan& ns = *mesh.span(to);
        glm::vec3 d(static_cast<float>(dx) * cs, static_cast<float>(dy) * cs,
                    (static_cast<float>(ns.floor) - static_cast<float>(from_floor)) * ch);
        len = glm::length(d) * mesh.cost_of(ns, filter);
        return from + d;
    };

    while (!sp.heap_empty()) {
        uint32_t cur = sp.pop();
        sp.at(cur).closed = true;
        SpanRef r = sp.at(cur).ref;
        if (r == goal) {
            reached = true;
            return cur;
        }
        if (++expanded >= filter.max_nodes) break;
        const glm::vec3 cur_pos = mesh.position_of(r);
        const uint16_t cur_floor = mesh.span(r)->floor;

        SpanRef card[4];
        SpanRef raw[4];
        for (int d = 0; d < 4; ++d) {
            raw[d] = mesh.neighbor(r, d);
            card[d] = enterable(raw[d]) ? raw[d] : k_invalid_span;
            if (card[d] == k_invalid_span) continue;
            float cost;
            glm::vec3 p = step_to(cur_pos, cur_floor, card[d], k_dir_dx[d], k_dir_dy[d], cost);
            try_edge(cur, card[d], p, cost, -1);
        }
        for (int d = 0; d < 4; ++d) {
            int e = (d + 1) & 3;
            if (card[d] == k_invalid_span || card[e] == k_invalid_span) continue;
            // Diagonal = both L-routes agree (see NavMesh::diagonal()), reusing the cardinals.
            SpanRef x = mesh.neighbor(card[d], e);
            if (x == k_invalid_span || mesh.neighbor(card[e], d) != x || !enterable(x)) continue;
            float cost;
            glm::vec3 p = step_to(cur_pos, cur_floor, x, k_dir_dx[d] + k_dir_dx[e], k_dir_dy[d] + k_dir_dy[e], cost);
            try_edge(cur, x, p, cost, -1);
        }
        if (!mesh.link_edges.empty()) {
            auto it = mesh.link_edges.find(r);
            if (it != mesh.link_edges.end()) {
                for (const LinkEdge& le : it->second) {
                    const OffMeshLink& l = mesh.links[le.link];
                    if (!filter.passes_area(l.area) || !enterable(le.to)) continue;
                    float len = glm::distance(l.start, l.end) * std::max(l.cost, 0.0f) * filter.area_cost[l.area];
                    try_edge(cur, le.to, mesh.position_of(le.to), len, static_cast<int16_t>(le.link));
                }
            }
        }
    }
    return best;
}

/**
 * @brief The span of component `comp` closest to `goal`, found through the coarse graph:
 *        tiles are scanned in rings of growing distance from the goal's tile, regions of the
 *        wrong component skipped by their label, and the scan stops once no closer ring can
 *        beat the best region found. Lets an unreachable query plan straight to the closest
 *        reachable point instead of flooding its whole component to discover it.
 */
inline SpanRef nearest_in_component_(const NavMesh& mesh, uint32_t comp, const glm::vec3& goal, const QueryFilter& filter) {
    const NavGridParams& p = mesh.params;
    const float tile_w = static_cast<float>(p.tile_size) * p.cs;
    glm::ivec2 gt(static_cast<int>(std::floor((goal.x - p.origin.x) / tile_w)),
                  static_cast<int>(std::floor((goal.y - p.origin.y) / tile_w)));
    gt = glm::clamp(gt, glm::ivec2(0), glm::ivec2(p.tiles_x - 1, p.tiles_y - 1));
    const int max_ring = std::max(p.tiles_x, p.tiles_y);
    float best_d = std::numeric_limits<float>::max();
    SpanRef best = k_invalid_span;
    for (int ring = 0; ring <= max_ring; ++ring) {
        // Any span in this ring is at least (ring - 1) tiles away in XY.
        float ring_min = static_cast<float>(std::max(ring - 1, 0)) * tile_w;
        if (ring_min * ring_min > best_d) break;
        for (int ty = gt.y - ring; ty <= gt.y + ring; ++ty) {
            for (int tx = gt.x - ring; tx <= gt.x + ring; ++tx) {
                if (std::max(std::abs(tx - gt.x), std::abs(ty - gt.y)) != ring) continue;
                const NavTile* t = mesh.tile_at(tx, ty);
                if (!t) continue;
                uint32_t ti = mesh.tile_index(tx, ty);
                const auto& comps = mesh.components[ti];
                bool any = false;
                for (uint32_t c : comps) any |= c == comp;
                if (!any) continue;
                for (uint32_t s = 0; s < t->spans.size(); ++s) {
                    const NavSpan& sp = t->spans[s];
                    if (sp.region >= comps.size() || comps[sp.region] != comp || !mesh.passable(sp, filter)) continue;
                    glm::vec3 d = mesh.position_of(make_ref(ti, s)) - goal;
                    float d2 = glm::dot(d, d);
                    if (d2 < best_d) {
                        best_d = d2;
                        best = make_ref(ti, s);
                    }
                }
            }
        }
    }
    return best;
}

/**
 * @brief Final point-level pass: drops every corner the surface lets the path cut (a clear
 *        raycast from the previous kept point to the next one, not crossing anything costlier
 *        than the three points' own spans). Span-level smoothing works within a bounded
 *        look-ahead and from cell centres; this removes the leftover kinks. Link endpoints stay.
 */
inline void shortcut_points_(const NavMesh& mesh, const QueryFilter& filter, NavPath& path) {
    if (path.points.size() < 3) return;
    std::vector<glm::vec3> pts;
    std::vector<uint8_t> fl;
    pts.push_back(path.points[0]);
    fl.push_back(path.flags[0]);
    for (std::size_t i = 1; i + 1 < path.points.size(); ++i) {
        const glm::vec3& a = pts.back();
        const glm::vec3& b = path.points[i];
        const glm::vec3& c = path.points[i + 1];
        bool keep = path.flags[i] != k_path_point_none || (fl.back() & k_path_point_link_start);
        if (!keep) {
            const glm::vec3 ext(mesh.params.cs, mesh.params.cs, mesh.params.ch * 2.0f + 0.05f);
            SpanRef ra = mesh.find_nearest(a, ext, &filter);
            SpanRef rb = mesh.find_nearest(b, ext, &filter);
            SpanRef rc = mesh.find_nearest(c, ext, &filter);
            keep = ra == k_invalid_span || rb == k_invalid_span || rc == k_invalid_span;
            if (!keep) {
                float max_cost = std::max({mesh.cost_of(*mesh.span(ra), filter), mesh.cost_of(*mesh.span(rb), filter),
                                           mesh.cost_of(*mesh.span(rc), filter)});
                NavRaycastHit hit;
                keep = !mesh.raycast(ra, a, c, filter, hit, max_cost * 1.0001f) || hit.last != rc;
            }
        }
        if (keep) {
            pts.push_back(b);
            fl.push_back(path.flags[i]);
        }
    }
    pts.push_back(path.points.back());
    fl.push_back(path.flags.back());
    path.points.swap(pts);
    path.flags.swap(fl);
}

} // namespace detail

/**
 * @brief Finds a path from `start` to `goal`. See the file doc for the stages.
 * @return out.status; out.points begin at `start` snapped to the surface.
 */
inline PathStatus find_path(const NavMesh& mesh, const glm::vec3& start, const glm::vec3& goal, const QueryFilter& filter,
                            NavPath& out, const PathQueryOptions& opts = {}, PathQueryScratch* scratch_ptr = nullptr) {
    PathQueryScratch& scratch = scratch_ptr ? *scratch_ptr : thread_scratch();
    out = NavPath{};
    glm::vec3 start_on, goal_on;
    SpanRef sref = mesh.find_nearest(start, opts.search_extents, &filter, &start_on);
    SpanRef gref = mesh.find_nearest(goal, opts.search_extents, &filter, &goal_on);
    if (sref == k_invalid_span || gref == k_invalid_span) return out.status = PathStatus::NoPath;

    bool same_component = mesh.component_of(sref) == mesh.component_of(gref);
    bool maybe_reachable = same_component || mesh.has_one_way_links;
    if (!maybe_reachable && !filter.allow_partial) return out.status = PathStatus::NoPath;

    // Unreachable for certain: aim at the reachable span nearest the goal instead, and report
    // the result as partial. (With one-way links a component mismatch proves nothing, so the
    // search runs against the real goal and partial results come from its closest node.)
    bool redirected = false;
    if (!same_component && !mesh.has_one_way_links) {
        SpanRef alt = detail::nearest_in_component_(mesh, mesh.component_of(sref), goal_on, filter);
        if (alt == k_invalid_span) return out.status = PathStatus::NoPath;
        gref = alt;
        goal_on = mesh.position_of(alt);
        same_component = true;
        redirected = true;
    }

    const float min_cost = detail::min_area_cost_(mesh, filter);
    const glm::vec3 goal_pos = mesh.position_of(gref);
    const StampedMap* corridor = nullptr;
    if (opts.use_corridor && same_component) {
        glm::ivec2 a(static_cast<int>(ref_tile(sref)) % mesh.params.tiles_x, static_cast<int>(ref_tile(sref)) / mesh.params.tiles_x);
        glm::ivec2 b(static_cast<int>(ref_tile(gref)) % mesh.params.tiles_x, static_cast<int>(ref_tile(gref)) / mesh.params.tiles_x);
        if (std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)) > 1 &&
            detail::plan_corridor_(mesh, mesh.region_of(sref), mesh.region_of(gref), min_cost, opts.corridor_margin, scratch)) {
            corridor = &scratch.corridor;
        }
    }

    bool reached = false;
    uint32_t expanded = 0;
    uint32_t end_node = detail::search_(mesh, sref, gref, goal_pos, filter, min_cost, corridor, scratch, reached, expanded);
    out.nodes_expanded = expanded;
    if (!reached && corridor) {
        end_node = detail::search_(mesh, sref, gref, goal_pos, filter, min_cost, nullptr, scratch, reached, expanded);
        out.nodes_expanded += expanded;
    }
    if (!reached && !filter.allow_partial) return out.status = PathStatus::NoPath;

    // Raw span path, start -> end.
    SearchSpace& sp = scratch.fine;
    scratch.refs.clear();
    scratch.via.clear();
    for (uint32_t n = end_node; n != StampedMap::k_none; n = sp.at(n).parent) {
        scratch.refs.push_back(sp.at(n).ref);
        scratch.via.push_back(sp.at(n).via_link);
    }
    std::reverse(scratch.refs.begin(), scratch.refs.end());
    std::reverse(scratch.via.begin(), scratch.via.end());
    const std::vector<SpanRef>& refs = scratch.refs;
    const std::vector<int16_t>& via = scratch.via;

    auto point_of = [&](std::size_t i) {
        if (i == 0) return glm::vec3(start_on.x, start_on.y, mesh.floor_height(refs[0], start_on));
        if (i + 1 == refs.size() && reached) return glm::vec3(goal_on.x, goal_on.y, mesh.floor_height(refs[i], goal_on));
        return mesh.position_of(refs[i]);
    };
    auto push = [&](const glm::vec3& p, uint8_t flag) {
        out.points.push_back(p);
        out.flags.push_back(flag);
    };

    push(point_of(0), k_path_point_none);
    const int lookahead = std::max(2, opts.max_smoothing_lookahead);
    std::size_t anchor = 0;
    while (anchor + 1 < refs.size()) {
        // A link traversal is never smoothed across: emit both ends.
        if (via[anchor + 1] >= 0) {
            out.flags.back() |= k_path_point_link_start;
            push(point_of(anchor + 1), k_path_point_link_end);
            ++anchor;
            continue;
        }
        std::size_t next = anchor + 1;
        if (opts.smooth) {
            glm::vec3 pa = point_of(anchor);
            float max_cost = mesh.cost_of(*mesh.span(refs[anchor]), filter);
            max_cost = std::max(max_cost, mesh.cost_of(*mesh.span(refs[anchor + 1]), filter));
            std::size_t limit = std::min(refs.size() - 1, anchor + static_cast<std::size_t>(lookahead));
            for (std::size_t j = anchor + 2; j <= limit; ++j) {
                if (via[j] >= 0) break;
                max_cost = std::max(max_cost, mesh.cost_of(*mesh.span(refs[j]), filter));
                NavRaycastHit hit;
                if (!mesh.raycast(refs[anchor], pa, point_of(j), filter, hit, max_cost * 1.0001f) || hit.last != refs[j]) break;
                next = j;
            }
        }
        push(point_of(next), k_path_point_none);
        anchor = next;
    }
    if (out.points.size() == 1) push(out.points[0], k_path_point_none); // start == goal span
    if (opts.smooth) detail::shortcut_points_(mesh, filter, out);
    for (std::size_t i = 1; i < out.points.size(); ++i) out.length += glm::distance(out.points[i - 1], out.points[i]);
    return out.status = (reached && !redirected) ? PathStatus::Success : PathStatus::Partial;
}

/** @brief One request in a batch for find_paths(). */
struct PathRequest {
    glm::vec3 start{0.0f};
    glm::vec3 goal{0.0f};
    QueryFilter filter;
    PathQueryOptions options;
    NavPath result;
};

/**
 * @brief Runs a batch of path queries across the job engine (inline when `jobs` is null),
 *        each worker reusing its own thread_scratch(). Blocks until all are done.
 */
inline void find_paths(const NavMesh& mesh, std::span<PathRequest> requests, coopa::job::JobEngine* jobs) {
    auto run = [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            PathRequest& r = requests[i];
            find_path(mesh, r.start, r.goal, r.filter, r.result, r.options);
        }
    };
    if (jobs && requests.size() > 1) {
        jobs->parallel_for_blocking(requests.size(), 1, run, 0, coopa::job::Priority::Normal,
                                    coopa::job::Priority::Normal);
    } else {
        run(0, requests.size());
    }
}

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_PATH_QUERY_H
