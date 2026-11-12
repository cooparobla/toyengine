/**
 * @file flow_field.h
 * @brief FlowField -- a per-span "which way to the goal" field for steering crowds, built over
 *        the same NavMesh the A* queries use.
 *
 * Integration is the Fast Marching Method (an Eikonal solve, not plain Dijkstra), so distance
 * grows isotropically -- a field over open ground points straight at the goal instead of in the
 * eight compass directions grid Dijkstra produces -- and it runs over the layered surface by
 * following span links, so a field on the ground floor flows up the stairs toward a goal on the
 * floor above. Each span's cost is its area cost (and an optional wall-proximity penalty that
 * keeps crowds off walls) times the cell size. Off-mesh links are Dijkstra edges in the same
 * march. Several goals may seed one field (nearest goal wins), and `max_distance` bounds the
 * march for large worlds.
 *
 * Directions are computed per span in a job-parallel pass: the negative gradient of the
 * distance field, falling back to the steepest-descent neighbour where the gradient is
 * unreliable (beside walls, at ridges where two routes tie). sample() blends the four nearest
 * spans bilinearly, so agents get a smooth vector anywhere on the surface.
 *
 * A FlowField is immutable once built and holds the NavMesh it was built on, so it stays
 * internally consistent while a newer mesh or field is being built in the background.
 */

#ifndef PHYSXCOOPA_NAV_FLOW_FIELD_H
#define PHYSXCOOPA_NAV_FLOW_FIELD_H

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
#include <queue>
#include <span>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/** @brief How much of the surface a FlowField integrates exactly. */
enum class FlowFieldMode : uint8_t {
    /** Exact when the reachable area is small (<= exact_tile_budget tiles), Hierarchical otherwise. */
    Auto,
    /** One fast-marching pass over everything reachable (bounded by required_points/max_distance). */
    Exact,
    /** Coarse portal routing for the whole world, exact integration only in the active tiles --
     *  see FlowField's class doc. Cost scales with where the followers are, not with the world. */
    Hierarchical,
};

/** @brief How a FlowField is built. */
struct FlowFieldSettings {
    FlowFieldMode mode = FlowFieldMode::Auto;
    /** @brief Auto: integrate exactly when the coarse pass reached at most this many tiles. */
    uint32_t exact_tile_budget = 64;
    /** @brief Hierarchical: tiles within this coarse distance of the goal are always integrated
     *         exactly, so the final approach -- where crowds converge -- is smooth. */
    float near_radius = 12.0f;
    /** @brief Hierarchical: tiles integrated ahead of each follower along its coarse route, so it
     *         stays inside the exact area between rebuilds. */
    int lookahead_tiles = 2;

    QueryFilter filter;
    /** @brief Stop marching past this distance (cost-weighted metres). 0 = whole component. */
    float max_distance = 0.0f;
    /** @brief How far a goal may be from the surface and still snap to it. */
    glm::vec3 goal_search_extents{2.0f, 2.0f, 4.0f};
    /**
     * @brief Points the field must cover -- typically the positions of the agents following it.
     *        Once the march has reached every one, it continues only to
     *        `farthest * required_margin_scale + required_margin` and stops: a chase field's cost
     *        then scales with how spread out its followers are, not with the size of the world.
     *        Empty = march the whole connected area (or to max_distance).
     */
    std::vector<glm::vec3> required_points;
    float required_margin = 6.0f;
    float required_margin_scale = 1.25f;
    /** @brief Whether an exact field stops once required_points are covered. Off = they only
     *         steer the hierarchical planner (and Auto's choice), the exact march runs to the end. */
    bool bound_exact_to_required = true;

    FlowFieldSettings() {
        filter.wall_penalty = 1.0f;          // crowds hug walls otherwise
        filter.wall_penalty_distance = 1.0f;
    }
};

/** @brief One FlowField::sample() result. */
struct FlowSample {
    bool valid = false;               /**< On the surface and reached by the field. */
    bool at_goal = false;             /**< Within the goal's own span. */
    glm::vec3 direction{0.0f};        /**< Unit XY direction to travel (z = 0). */
    float distance = std::numeric_limits<float>::max(); /**< Cost-weighted distance to the goal. */
    int16_t link = -1;                /**< Off-mesh link to take from here, or -1. */
    SpanRef ref = k_invalid_span;     /**< Span the sample point is on. */
    /** @brief The point lies outside the exactly integrated (active) tiles of a hierarchical
     *         field: `direction` heads for the best portal of its region on the coarse route. */
    bool coarse = false;
};

/**
 * @class FlowField
 *
 * Hierarchical mode (open worlds). Integrating the whole reachable surface costs time in
 * proportion to the world; a crowd only needs directions where it is. So:
 *   1. Coarse pass: Dijkstra from the goal over the portal graph (NavPortal: runs of crossings
 *      between two regions over a tile border; edges = straight lines across a region and the
 *      crossings themselves). Every portal gets a cost-to-goal. It stops shortly after every
 *      follower's region is reached. Thousands of nodes, not millions of spans.
 *   2. Active set: the followers' tiles, `lookahead_tiles` tiles further along each one's coarse
 *      route, and every tile within `near_radius` of the goal.
 *   3. One exact fast march over the active tiles only, seeded at the goal (when active) and at
 *      the active set's boundary with the coarse estimate of the cell just outside it. Inside,
 *      directions are consistent; the rest of the world is summarised by the coarse layer.
 *   4. Outside the active tiles, sample() still answers, from the coarse layer: head for the best
 *      portal of the current region (FlowSample::coarse). NavSystem rebuilds the field when a
 *      follower ends up there, so the active window rolls along with the crowd.
 * Coarse estimates are scaled by the worst-case wall penalty, so at the boundary they never
 * undercut the exact values next to them -- crowds don't leak out of the exact area.
 */
class FlowField {
public:
    /**
     * @brief Builds a field toward `goals` over `mesh`. The direction pass is parallel on
     *        `jobs` when given; the march itself is serial (callers run whole builds as jobs --
     *        see NavSystem -- so several fields build at once).
     * @return The field; it is valid() if at least one goal snapped to the surface.
     */
    static std::shared_ptr<const FlowField> build(std::shared_ptr<const NavMesh> mesh, std::span<const glm::vec3> goals,
                                                  const FlowFieldSettings& settings = {},
                                                  coopa::job::JobEngine* jobs = nullptr) {
        auto f = std::shared_ptr<FlowField>(new FlowField());
        f->mesh_ = std::move(mesh);
        f->settings_ = settings;
        f->goals_.assign(goals.begin(), goals.end());
        if (f->mesh_) {
            f->begin_march_();
            f->march_step_(std::numeric_limits<std::size_t>::max());
            f->end_march_();
            f->compute_directions_(jobs);
        }
        return f;
    }

    bool valid() const { return seeded_; }
    const NavMesh& mesh() const { return *mesh_; }
    const std::shared_ptr<const NavMesh>& mesh_ptr() const { return mesh_; }
    const std::vector<glm::vec3>& goals() const { return goals_; }
    const FlowFieldSettings& settings() const { return settings_; }
    /** @brief Spans the march reached. */
    std::size_t reached_count() const { return reached_; }
    /** @brief Whether this field was built hierarchically (see the class doc). */
    bool hierarchical() const { return hierarchical_; }
    /** @brief Tiles integrated exactly. */
    std::size_t active_tile_count() const { return active_count_; }
    /** @brief Tiles the coarse pass reached. */
    std::size_t coarse_tile_count() const { return coarse_count_; }
    /** @brief Whether tile `t` was integrated exactly (always true for an exact field's tiles). */
    bool tile_active(uint32_t t) const { return active_.empty() ? (t < tiles_.size() && tiles_[t]) : (t < active_.size() && active_[t]); }

    /**
     * @brief Coarse-layer estimate at `pos` on span `r`: cost to the goal through the best
     *        portal of r's region (or straight to the goal inside the goal's region). Returns
     *        infinity where the coarse pass didn't reach. `exit_mid` gets the portal midpoint.
     */
    float coarse_estimate(SpanRef r, const glm::vec3& pos, glm::vec3* exit_mid = nullptr) const {
        if (r == k_invalid_span || portal_cost_.empty()) return k_inf;
        return region_estimate_(mesh_->region_of(r), pos, exit_mid, nullptr);
    }

    /** @brief coarse_estimate() for a whole region (e.g. from its centroid, for debug views). */
    float coarse_estimate_region(RegionRef reg, const glm::vec3& pos, glm::vec3* exit_mid = nullptr) const {
        if (portal_cost_.empty()) return k_inf;
        return region_estimate_(reg, pos, exit_mid, nullptr);
    }

    /** @brief Distance and raw direction stored at one span (no blending). */
    bool span_value(SpanRef r, float& dist, glm::vec2& dir) const {
        const TileData* td = tile_data_(r);
        if (!td) return false;
        uint32_t s = ref_span(r);
        dist = td->dist[s];
        dir = td->dir[s];
        return dist < k_inf;
    }

    /** @brief Samples at a world position on (or within `search_z` above) the surface. */
    FlowSample sample(const glm::vec3& pos, float search_z = 2.0f) const {
        if (!mesh_) return {};
        return sample(mesh_->locate(pos, search_z), pos);
    }

    /** @brief Samples at `pos` known to be on span `r` (saves the locate). */
    FlowSample sample(SpanRef r, const glm::vec3& pos) const {
        FlowSample out;
        out.ref = r;
        const TileData* td = tile_data_(r);
        float d0 = td ? td->dist[ref_span(r)] : k_inf;
        if (!(d0 < k_inf)) {
            // Outside the exact area: the coarse layer, if this field has one.
            glm::vec3 exit(0.0f);
            float e = hierarchical_ ? coarse_estimate(r, pos, &exit) : k_inf;
            if (!(e < k_inf)) return out;
            glm::vec2 v = glm::vec2(exit) - glm::vec2(pos);
            float l = glm::length(v);
            out.valid = true;
            out.coarse = true;
            out.distance = e;
            out.direction = l > 1e-4f ? glm::vec3(v / l, 0.0f) : glm::vec3(0.0f);
            return out;
        }
        uint32_t s = ref_span(r);
        out.valid = true;
        out.distance = d0;
        uint8_t flags = td->flags[s];
        if (flags & k_flag_link) {
            out.link = td->link[s];
            out.direction = glm::vec3(td->dir[s], 0.0f);
            return out;
        }
        if (flags & k_flag_goal) {
            // Steer onto the exact goal point rather than the cell centre.
            out.at_goal = true;
            glm::vec3 g = goals_[td->goal[s]];
            glm::vec2 v(g.x - pos.x, g.y - pos.y);
            float l = glm::length(v);
            out.direction = l > 1e-4f ? glm::vec3(v / l, 0.0f) : glm::vec3(0.0f);
            out.distance = l;
            return out;
        }

        // Bilinear blend with the three neighbours toward pos within the cell.
        const NavMesh& m = *mesh_;
        glm::ivec2 c = m.cell_of(r);
        float fx = (pos.x - m.params.origin.x) / m.params.cs - static_cast<float>(c.x) - 0.5f;
        float fy = (pos.y - m.params.origin.y) / m.params.cs - static_cast<float>(c.y) - 0.5f;
        int dx = fx >= 0.0f ? 2 : 0;
        int dy = fy >= 0.0f ? 1 : 3;
        float ax = std::min(std::fabs(fx), 0.5f), ay = std::min(std::fabs(fy), 0.5f);
        SpanRef nx = m.neighbor(r, dx), ny = m.neighbor(r, dy);
        SpanRef nxy = nx != k_invalid_span ? m.neighbor(nx, dy) : (ny != k_invalid_span ? m.neighbor(ny, dx) : k_invalid_span);
        const SpanRef refs[4] = {r, nx, ny, nxy};
        const float w[4] = {(1.0f - ax) * (1.0f - ay), ax * (1.0f - ay), (1.0f - ax) * ay, ax * ay};
        glm::vec2 dir(0.0f);
        float dist = 0.0f, wsum = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (refs[i] == k_invalid_span) continue;
            float d;
            glm::vec2 v;
            if (!span_value(refs[i], d, v)) continue;
            const TileData* t = tile_data_(refs[i]);
            if (t->flags[ref_span(refs[i])] & k_flag_link) continue; // don't drag neighbours into a link
            dir += v * w[i];
            dist += d * w[i];
            wsum += w[i];
        }
        float l = glm::length(dir);
        out.direction = l > 1e-5f ? glm::vec3(dir / l, 0.0f) : glm::vec3(td->dir[s], 0.0f);
        out.distance = wsum > 0.0f ? dist / wsum : d0;
        return out;
    }

private:
    static constexpr float k_inf = std::numeric_limits<float>::infinity();
    static constexpr uint8_t k_flag_accepted = 1u << 0;
    static constexpr uint8_t k_flag_goal = 1u << 1;
    static constexpr uint8_t k_flag_link = 1u << 2;
    static constexpr uint8_t k_flag_required = 1u << 3;

    struct TileData {
        uint32_t revision = 0;
        std::vector<float> dist;
        std::vector<glm::vec2> dir;
        std::vector<uint8_t> flags;
        std::vector<int16_t> link;   /**< Off-mesh link from this span when k_flag_link. */
        std::vector<uint16_t> goal;  /**< Goal index when k_flag_goal. */
    };

    struct Item {
        float d;
        SpanRef r;
        bool operator>(const Item& o) const { return d > o.d; }
    };
    /**
     * @class OpenList
     * @brief The "untidy" bucket queue of O(N) fast marching (Yatziv, Bartesaghi and Sapiro
     *        2006): items are binned by distance into buckets `width` wide and popped bucket by
     *        bucket, in no particular order within one. Accept order is off by at most `width`
     *        -- set well under one cell's step cost, the extra error is far below FMM's own --
     *        and push/pop are O(1) instead of a heap's O(log n). A ring of buckets covers the
     *        live window; anything beyond it (a long off-mesh link) waits in an overflow list.
     */
    class OpenList {
    public:
        explicit OpenList(float width = 0.05f) : width_(width), buckets_(k_ring) {}

        void set_width(float w) { width_ = std::max(w, 1e-4f); }
        bool empty() const { return count_ == 0; }

        void push(const Item& it) {
            ++count_;
            int64_t b = static_cast<int64_t>(it.d / width_);
            if (b < cur_) b = cur_;
            if (b >= cur_ + static_cast<int64_t>(k_ring)) {
                overflow_.push_back(it);
                return;
            }
            buckets_[static_cast<std::size_t>(b) & (k_ring - 1)].push_back(it);
        }

        Item pop() {
            for (;;) {
                auto& bucket = buckets_[static_cast<std::size_t>(cur_) & (k_ring - 1)];
                if (!bucket.empty()) {
                    Item it = bucket.back();
                    bucket.pop_back();
                    --count_;
                    return it;
                }
                ++cur_;
                if (in_ring_() == 0 && !overflow_.empty()) refill_();
            }
        }

    private:
        static constexpr std::size_t k_ring = 4096; // power of two

        std::size_t in_ring_() const { return count_ - overflow_.size(); }

        /** @brief The ring ran dry: jump to the nearest overflow item and re-bin what fits. */
        void refill_() {
            float lo = std::numeric_limits<float>::max();
            for (const Item& it : overflow_) lo = std::min(lo, it.d);
            cur_ = std::max(cur_, static_cast<int64_t>(lo / width_));
            std::vector<Item> rest;
            for (const Item& it : overflow_) {
                int64_t b = std::max(cur_, static_cast<int64_t>(it.d / width_));
                if (b >= cur_ + static_cast<int64_t>(k_ring)) rest.push_back(it);
                else buckets_[static_cast<std::size_t>(b) & (k_ring - 1)].push_back(it);
            }
            overflow_.swap(rest);
        }

        float width_;
        std::vector<std::vector<Item>> buckets_;
        std::vector<Item> overflow_;
        int64_t cur_ = 0;
        std::size_t count_ = 0;
    };

    /** @brief Radius, in cells, of the disk around each goal initialised with exact distances. */
    static constexpr int k_seed_radius = 4;

    FlowField() = default;

    /**
     * @brief Initialises the spans within k_seed_radius cells of a goal span with their exact
     *        straight-line distance (when the surface between them is clear). First-order fast
     *        marching from a single point overestimates diagonal distances by up to ~15% -- the
     *        error is born next to the source, where the wavefront is most curved -- and starting
     *        the march from an exact disk instead removes most of it.
     */
    void seed_disk_(SpanRef goal_ref, const glm::vec3& goal_on, std::size_t gi, OpenList& open) {
        const NavMesh& m = *mesh_;
        const QueryFilter& filter = settings_.filter;
        std::vector<SpanRef> frontier{goal_ref}, next;
        std::vector<SpanRef> visited{goal_ref};
        const glm::ivec2 gc = m.cell_of(goal_ref);
        const float base = m.cost_of(*m.span(goal_ref), filter);
        for (int ring = 0; ring < k_seed_radius; ++ring) {
            next.clear();
            for (SpanRef r : frontier) {
                for (int d = 0; d < 4; ++d) {
                    SpanRef n = m.neighbor(r, d);
                    if (n == k_invalid_span || !allowed_(ref_tile(n)) ||
                        std::find(visited.begin(), visited.end(), n) != visited.end()) continue;
                    visited.push_back(n);
                    const NavSpan& ns = *m.span(n);
                    if (!m.passable(ns, filter)) continue;
                    glm::ivec2 c = m.cell_of(n);
                    if ((c.x - gc.x) * (c.x - gc.x) + (c.y - gc.y) * (c.y - gc.y) > k_seed_radius * k_seed_radius) continue;
                    next.push_back(n);
                    // Exact only over uniform cost and a clear straight line.
                    if (m.cost_of(ns, filter) != base) continue;
                    glm::vec3 p = m.position_of(n);
                    NavRaycastHit hit;
                    if (!m.raycast(goal_ref, goal_on, p, filter, hit, base * 1.0001f) || hit.last != n) continue;
                    float dist = glm::length(p - goal_on) * base;
                    TileData& td = ensure_tile_(ref_tile(n));
                    uint32_t s = ref_span(n);
                    if (dist < td.dist[s]) {
                        td.dist[s] = dist;
                        td.flags[s] &= static_cast<uint8_t>(~k_flag_goal);
                        td.goal[s] = static_cast<uint16_t>(gi);
                        open.push(Item{dist, n});
                    }
                }
            }
            frontier.swap(next);
        }
    }

    const TileData* tile_data_(SpanRef r) const {
        if (r == k_invalid_span) return nullptr;
        uint32_t t = ref_tile(r);
        if (t >= tiles_.size() || !tiles_[t]) return nullptr;
        const TileData* td = tiles_[t].get();
        return ref_span(r) < td->dist.size() ? td : nullptr;
    }

    TileData& ensure_tile_(uint32_t t) {
        if (!tiles_[t]) {
            auto td = std::make_unique<TileData>();
            const NavTile& nt = *mesh_->tiles[t];
            std::size_t n = nt.spans.size();
            td->revision = nt.revision;
            td->dist.assign(n, k_inf);
            td->dir.assign(n, glm::vec2(0.0f));
            td->flags.assign(n, 0);
            td->link.assign(n, -1);
            td->goal.assign(n, 0);
            tiles_[t] = std::move(td);
        }
        return *tiles_[t];
    }

    /** @brief March state, alive only while the field is being built (see FlowFieldBuilder). */
    struct MarchState {
        OpenList open;
        /** @brief Incoming link edges: link_edges is keyed by the span a traversal leaves, the
         *         march needs "which spans can reach me through a link". */
        std::unordered_map<SpanRef, std::vector<std::pair<SpanRef, uint16_t>>> incoming;
        float max_d = std::numeric_limits<float>::infinity();
        std::size_t required_left = 0;
    };

    void begin_march_() {
        const NavMesh& m = *mesh_;
        const QueryFilter& filter = settings_.filter;
        tiles_.resize(m.tiles.size());
        march_ = std::make_unique<MarchState>();
        OpenList& open = march_->open;
        if (settings_.mode != FlowFieldMode::Exact) plan_hierarchy_();
        // Bucket width: a tenth of the cheapest step, so the untidy order's error stays well
        // under the scheme's own first-order error.
        float min_cost = std::numeric_limits<float>::max();
        for (uint32_t a = 1; a < k_max_areas; ++a) {
            if ((filter.area_mask >> a) & 1ull) min_cost = std::min(min_cost, filter.area_cost[a]);
        }
        if (!(min_cost > 0.0f) || min_cost == std::numeric_limits<float>::max()) min_cost = 1.0f;
        open.set_width(0.1f * min_cost * m.params.cs);

        for (std::size_t gi = 0; gi < goals_.size() && gi < 0xFFFF; ++gi) {
            glm::vec3 on;
            SpanRef g = m.find_nearest(goals_[gi], settings_.goal_search_extents, &filter, &on);
            if (g == k_invalid_span) continue;
            seeded_ = true;
            if (!allowed_(ref_tile(g))) continue;
            TileData& td = ensure_tile_(ref_tile(g));
            uint32_t s = ref_span(g);
            glm::vec3 c = m.position_of(g);
            float d = glm::length(glm::vec2(goals_[gi]) - glm::vec2(c)) * m.cost_of(*m.span(g), filter);
            if (d < td.dist[s]) {
                td.dist[s] = d;
                td.flags[s] |= k_flag_goal;
                td.goal[s] = static_cast<uint16_t>(gi);
                open.push(Item{d, g});
            }
            seed_disk_(g, on, gi, open);
        }
        for (const auto& [from, edges] : m.link_edges) {
            for (const LinkEdge& e : edges) march_->incoming[e.to].push_back({from, e.link});
        }
        march_->max_d = settings_.max_distance > 0.0f ? settings_.max_distance : k_inf;
        if (hierarchical_) seed_boundary_(open);
        const bool bound = hierarchical_ || settings_.bound_exact_to_required;
        for (const glm::vec3& p : bound ? settings_.required_points : std::vector<glm::vec3>{}) {
            SpanRef r = m.locate(p);
            if (r == k_invalid_span || !allowed_(ref_tile(r))) continue;
            TileData& td = ensure_tile_(ref_tile(r));
            uint8_t& f = td.flags[ref_span(r)];
            if (f & k_flag_required) continue;
            f |= k_flag_required;
            ++march_->required_left;
        }
    }

    /** @brief Accepts up to `budget` spans. Returns true once the march is complete. */
    bool march_step_(std::size_t budget) {
        const NavMesh& m = *mesh_;
        const QueryFilter& filter = settings_.filter;
        const float cs = m.params.cs;
        OpenList& open = march_->open;
        auto accepted_dist = [&](SpanRef r) -> float {
            if (r == k_invalid_span) return k_inf;
            const TileData* td = tiles_[ref_tile(r)].get();
            if (!td) return k_inf;
            uint32_t s = ref_span(r);
            return (td->flags[s] & k_flag_accepted) ? td->dist[s] : k_inf;
        };

        std::size_t accepted = 0;
        while (!open.empty()) {
            if (accepted >= budget) return false;
            Item it = open.pop();
            TileData& td = *tiles_[ref_tile(it.r)];
            uint32_t s = ref_span(it.r);
            if ((td.flags[s] & k_flag_accepted) || it.d > td.dist[s]) continue;
            if (it.d > march_->max_d) break;
            td.flags[s] |= k_flag_accepted;
            ++reached_;
            ++accepted;
            if ((td.flags[s] & k_flag_required) && --march_->required_left == 0) {
                // Every follower is covered: finish a margin past the farthest one.
                march_->max_d = std::min(march_->max_d, it.d * settings_.required_margin_scale + settings_.required_margin);
            }
            const NavSpan& cur = *m.span(it.r);
            const float cur_z = m.params.floor_z(cur.floor);

            for (int d = 0; d < 4; ++d) {
                SpanRef n = cur.nbr[d];
                if (n == k_invalid_span || !allowed_(ref_tile(n))) continue;
                const NavSpan& ns = *m.span(n);
                if (!m.passable(ns, filter)) continue;
                TileData& nd = ensure_tile_(ref_tile(n));
                uint32_t nsi = ref_span(n);
                if (nd.flags[nsi] & k_flag_accepted) continue;
                // Eikonal update from n's accepted neighbours on each axis.
                float a = std::min(accepted_dist(ns.nbr[0]), accepted_dist(ns.nbr[2]));
                float b = std::min(accepted_dist(ns.nbr[1]), accepted_dist(ns.nbr[3]));
                const float cost = m.cost_of(ns, filter);
                float c = cost * cs;
                float t;
                if (!(a < k_inf) || !(b < k_inf) || std::fabs(a - b) >= c) {
                    t = std::min(a, b) + c;
                } else {
                    t = 0.5f * (a + b + std::sqrt(2.0f * c * c - (a - b) * (a - b)));
                }
                // Steps change floor height: charge the climb too.
                t += std::fabs(m.params.floor_z(ns.floor) - cur_z) * cost;
                if (t < nd.dist[nsi]) {
                    nd.dist[nsi] = t;
                    nd.flags[nsi] &= static_cast<uint8_t>(~k_flag_link);
                    open.push(Item{t, n});
                }
            }
            if (!march_->incoming.empty()) {
                auto inc = march_->incoming.find(it.r);
                if (inc != march_->incoming.end()) {
                    for (const auto& [from, link] : inc->second) {
                        const OffMeshLink& l = m.links[link];
                        if (!allowed_(ref_tile(from)) || !filter.passes_area(l.area) || !m.passable(*m.span(from), filter)) continue;
                        TileData& fd = ensure_tile_(ref_tile(from));
                        uint32_t fs = ref_span(from);
                        if (fd.flags[fs] & k_flag_accepted) continue;
                        float t = it.d + glm::distance(l.start, l.end) * std::max(l.cost, 0.0f) * filter.area_cost[l.area];
                        if (t < fd.dist[fs]) {
                            fd.dist[fs] = t;
                            fd.flags[fs] |= k_flag_link;
                            fd.link[fs] = static_cast<int16_t>(link);
                            open.push(Item{t, from});
                        }
                    }
                }
            }
        }
        return true;
    }

    /** @brief Drops the march state; spans valued but never accepted (beyond max_distance) are
     *         unreached. */
    void end_march_() {
        march_.reset();
        for (auto& td : tiles_) {
            if (!td) continue;
            for (std::size_t i = 0; i < td->dist.size(); ++i) {
                if (!(td->flags[i] & k_flag_accepted)) td->dist[i] = k_inf;
            }
        }
    }

    bool allowed_(uint32_t t) const { return active_.empty() || (t < active_.size() && active_[t]); }

    /**
     * @brief Coarse estimate from `pos` in region `reg`: the best of its reached portals
     *        (straight line to the portal + the portal's cost) and, in a goal's own region, the
     *        straight line to the goal. `exit_portal` gets the portal (-1 = the goal itself).
     */
    float region_estimate_(RegionRef reg, const glm::vec3& pos, glm::vec3* exit_mid, int* exit_portal) const {
        float best = goal_region_estimate_(reg, pos, exit_mid);
        if (exit_portal) *exit_portal = -1;
        uint32_t t = ref_tile(reg);
        if (t >= portal_cost_.size() || portal_cost_[t].empty()) return best;
        const NavTile& tile = *mesh_->tiles[t];
        const auto& costs = portal_cost_[t];
        for (uint16_t pi : tile.regions[ref_span(reg)].portals) {
            if (!(costs[pi] < k_inf)) continue;
            const glm::vec3& mid = tile.portals[pi].mid;
            float e = glm::distance(pos, mid) * coarse_scale_ + costs[pi];
            if (e < best) {
                best = e;
                if (exit_mid) *exit_mid = mid;
                if (exit_portal) *exit_portal = pi;
            }
        }
        return best;
    }

    /** @brief Inside a goal's own region the estimate is the straight distance to that goal. */
    float goal_region_estimate_(RegionRef reg, const glm::vec3& pos, glm::vec3* exit_mid) const {
        float best = k_inf;
        for (const GoalSpan& g : goal_spans_) {
            if (mesh_->region_of(g.ref) != reg) continue;
            float e = glm::distance(pos, g.pos) * coarse_scale_;
            if (e < best) {
                best = e;
                if (exit_mid) *exit_mid = g.pos;
            }
        }
        return best;
    }

    /**
     * @brief Coarse pass + active-set choice (steps 1 and 2 of the class doc). Leaves the field
     *        exact (active_ empty) when the mode is Auto and the coarse pass reached few tiles.
     */
    void plan_hierarchy_() {
        const NavMesh& m = *mesh_;
        const QueryFilter& filter = settings_.filter;
        const std::size_t ntiles = m.tiles.size();
        const float cs = m.params.cs;
        const float tile_w = static_cast<float>(m.params.tile_size) * cs;
        coarse_scale_ = 1.0f + std::max(filter.wall_penalty, 0.0f);

        for (const glm::vec3& g : goals_) {
            glm::vec3 on;
            SpanRef r = m.find_nearest(g, settings_.goal_search_extents, &filter, &on);
            if (r != k_invalid_span) goal_spans_.push_back(GoalSpan{r, on});
        }
        if (goal_spans_.empty()) return;

        // Followers: their spans and regions.
        std::vector<std::pair<SpanRef, glm::vec3>> followers;
        std::unordered_map<RegionRef, uint8_t> pending; // follower regions not yet reached
        for (const glm::vec3& p : settings_.required_points) {
            SpanRef r = m.locate(p);
            if (r == k_invalid_span) continue;
            followers.push_back({r, p});
            pending[m.region_of(r)] = 1;
        }
        for (const GoalSpan& g : goal_spans_) pending.erase(m.region_of(g.ref));

        // 1. Dijkstra over portals. Node = (tile << 16) | portal.
        portal_cost_.assign(ntiles, {});
        auto costs_of = [&](uint32_t t) -> std::vector<float>& {
            auto& v = portal_cost_[t];
            if (v.empty() && m.tiles[t] && !m.tiles[t]->portals.empty()) {
                v.assign(m.tiles[t]->portals.size(), k_inf);
                ++coarse_count_;
            }
            return v;
        };
        struct Node {
            float d;
            uint32_t id;
            bool operator>(const Node& o) const { return d > o.d; }
        };
        std::priority_queue<Node, std::vector<Node>, std::greater<Node>> heap;
        auto relax = [&](uint32_t t, uint16_t p, float d) {
            auto& v = costs_of(t);
            if (p < v.size() && d < v[p]) {
                v[p] = d;
                heap.push(Node{d, (t << 16) | p});
            }
        };
        for (const GoalSpan& g : goal_spans_) {
            uint32_t t = ref_tile(g.ref);
            const NavTile& tile = *m.tiles[t];
            for (uint16_t p : tile.regions[m.span(g.ref)->region].portals) {
                relax(t, p, glm::distance(g.pos, tile.portals[p].mid) * coarse_scale_);
            }
        }
        float limit = pending.empty() ? 0.0f : k_inf;
        if (pending.empty()) limit = settings_.near_radius + 2.0f * tile_w;
        const float cross = cs * coarse_scale_;
        while (!heap.empty()) {
            Node nd = heap.top();
            heap.pop();
            uint32_t t = nd.id >> 16;
            uint16_t p = static_cast<uint16_t>(nd.id & 0xFFFFu);
            if (nd.d > portal_cost_[t][p]) continue;
            if (nd.d > limit) break;
            const NavTile& tile = *m.tiles[t];
            const NavPortal& port = tile.portals[p];
            if (!pending.empty() && pending.erase(make_ref(t, port.region)) && pending.empty()) {
                // Every follower region is routed: finish a margin further, and far enough to
                // cover the exact zone around the goal.
                limit = std::max(nd.d * settings_.required_margin_scale + settings_.required_margin + 2.0f * tile_w,
                                 settings_.near_radius + 2.0f * tile_w);
            }
            for (uint16_t q : tile.regions[port.region].portals) {
                if (q != p) relax(t, q, nd.d + glm::distance(port.mid, tile.portals[q].mid) * coarse_scale_);
            }
            uint16_t mirror = m.mirror_portal(t, p);
            if (mirror != 0xFFFF) relax(ref_tile(port.to), mirror, nd.d + cross);
        }

        if (settings_.mode == FlowFieldMode::Auto && coarse_count_ <= settings_.exact_tile_budget) {
            portal_cost_.clear(); // small world: integrate everything exactly
            goal_spans_.clear();
            coarse_count_ = 0;
            return;
        }

        // 2. Active set.
        hierarchical_ = true;
        active_.assign(ntiles, 0);
        auto activate = [&](uint32_t t) {
            if (t < ntiles && m.tiles[t] && !active_[t]) {
                active_[t] = 1;
                ++active_count_;
            }
        };
        for (const GoalSpan& g : goal_spans_) activate(ref_tile(g.ref));
        for (uint32_t t = 0; t < ntiles; ++t) {
            const auto& v = portal_cost_[t];
            for (float c : v) {
                if (c <= settings_.near_radius * coarse_scale_) {
                    activate(t);
                    break;
                }
            }
        }
        for (const auto& [ref, pos] : followers) {
            activate(ref_tile(ref));
            // Walk the coarse route a few tiles ahead.
            RegionRef reg = m.region_of(ref);
            glm::vec3 at = pos;
            for (int step = 0; step < settings_.lookahead_tiles; ++step) {
                int exit = -1;
                if (!(region_estimate_(reg, at, nullptr, &exit) < k_inf) || exit < 0) break; // goal region: done
                const NavPortal& port = m.tiles[ref_tile(reg)]->portals[static_cast<uint16_t>(exit)];
                activate(ref_tile(port.to));
                reg = port.to;
                at = port.mid;
            }
        }
    }

    /**
     * @brief Step 3's boundary seeds: every active span with a neighbour in an inactive tile
     *        starts at that neighbour's coarse estimate plus the step to it.
     */
    void seed_boundary_(OpenList& open) {
        const NavMesh& m = *mesh_;
        const QueryFilter& filter = settings_.filter;
        const float cs = m.params.cs;
        for (uint32_t t = 0; t < active_.size(); ++t) {
            if (!active_[t]) continue;
            const NavTile& tile = *m.tiles[t];
            for (uint32_t s = 0; s < tile.spans.size(); ++s) {
                const NavSpan& sp = tile.spans[s];
                if (!m.passable(sp, filter)) continue;
                float best = k_inf;
                for (int d = 0; d < 4; ++d) {
                    SpanRef n = sp.nbr[d];
                    if (n == k_invalid_span || allowed_(ref_tile(n))) continue;
                    float e = coarse_estimate(n, m.position_of(n), nullptr);
                    if (e < k_inf) best = std::min(best, e + cs * m.cost_of(sp, filter));
                }
                if (!(best < k_inf)) continue;
                TileData& td = ensure_tile_(t);
                if (best < td.dist[s]) {
                    td.dist[s] = best;
                    open.push(Item{best, make_ref(t, s)});
                }
            }
        }
    }

    std::vector<uint32_t> touched_tiles_() const {
        std::vector<uint32_t> touched;
        for (uint32_t t = 0; t < tiles_.size(); ++t) {
            if (tiles_[t]) touched.push_back(t);
        }
        return touched;
    }

    void compute_directions_(coopa::job::JobEngine* jobs) {
        std::vector<uint32_t> touched = touched_tiles_();
        auto run = [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) directions_for_tile_(touched[i]);
        };
        if (jobs && touched.size() > 1) {
            jobs->parallel_for_blocking(touched.size(), 1, run, 0, coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        }
        else run(0, touched.size());
    }

    void directions_for_tile_(uint32_t t) {
        const NavMesh& m = *mesh_;
        TileData& td = *tiles_[t];
        const float cs = m.params.cs;
        auto dist = [&](SpanRef r) -> float {
            const TileData* d = tile_data_(r);
            return d ? d->dist[ref_span(r)] : k_inf;
        };
        for (uint32_t s = 0; s < td.dist.size(); ++s) {
            float d0 = td.dist[s];
            if (!(d0 < k_inf)) continue;
            SpanRef r = make_ref(t, s);
            glm::vec3 p0 = m.position_of(r);
            if (td.flags[s] & k_flag_link) {
                const OffMeshLink& l = m.links[td.link[s]];
                bool reverse = m.links[td.link[s]].start_ref != r && l.bidirectional;
                glm::vec2 v = reverse ? glm::vec2(l.start - l.end) : glm::vec2(l.end - l.start);
                float len = glm::length(v);
                td.dir[s] = len > 1e-5f ? v / len : glm::vec2(0.0f);
                continue;
            }
            if (td.flags[s] & k_flag_goal) {
                glm::vec2 v = glm::vec2(goals_[td.goal[s]]) - glm::vec2(p0);
                float len = glm::length(v);
                td.dir[s] = len > 1e-5f ? v / len : glm::vec2(0.0f);
                continue;
            }
            // Steepest-descent neighbour over 8 directions (diagonals without corner cutting).
            SpanRef card[4];
            for (int d = 0; d < 4; ++d) card[d] = m.neighbor(r, d);
            float best_d = d0;
            glm::vec2 best_dir(0.0f);
            auto consider = [&](SpanRef n) {
                if (n == k_invalid_span) return;
                float dn = dist(n);
                if (dn < best_d) {
                    best_d = dn;
                    glm::vec2 v = glm::vec2(m.position_of(n)) - glm::vec2(p0);
                    best_dir = v / glm::length(v);
                }
            };
            for (int d = 0; d < 4; ++d) consider(card[d]);
            for (int d = 0; d < 4; ++d) {
                int e = (d + 1) & 3;
                if (card[d] != k_invalid_span && card[e] != k_invalid_span) consider(m.diagonal(r, d, e));
            }
            // Central/one-sided differences per axis.
            auto axis_grad = [&](SpanRef lo, SpanRef hi) -> float {
                float dl = lo != k_invalid_span ? dist(lo) : k_inf;
                float dh = hi != k_invalid_span ? dist(hi) : k_inf;
                bool hl = dl < k_inf, hh = dh < k_inf;
                if (hl && hh) return (dh - dl) / (2.0f * cs);
                if (hh) return std::min(dh - d0, 0.0f) / cs; // only descend into a known side
                if (hl) return std::max(d0 - dl, 0.0f) / cs;
                return 0.0f;
            };
            glm::vec2 g(axis_grad(card[0], card[2]), axis_grad(card[3], card[1]));
            float gl = glm::length(g);
            glm::vec2 dir = gl > 1e-6f ? -g / gl : best_dir;
            if (best_d < d0 && glm::dot(dir, best_dir) < 0.5f) dir = best_dir;
            // Never point straight into a wall: drop a component whose side has no link.
            if ((dir.x < -0.2f && card[0] == k_invalid_span) || (dir.x > 0.2f && card[2] == k_invalid_span) ||
                (dir.y > 0.2f && card[1] == k_invalid_span) || (dir.y < -0.2f && card[3] == k_invalid_span)) {
                if (glm::dot(best_dir, best_dir) > 0.0f) dir = best_dir;
            }
            td.dir[s] = dir;
        }
    }

    std::shared_ptr<const NavMesh> mesh_;
    FlowFieldSettings settings_;
    std::vector<glm::vec3> goals_;
    std::vector<std::unique_ptr<TileData>> tiles_;
    std::size_t reached_ = 0;
    bool seeded_ = false;
    std::unique_ptr<MarchState> march_;

    // Hierarchical state (see the class doc).
    bool hierarchical_ = false;
    std::vector<std::vector<float>> portal_cost_;  /**< Per tile, per portal; empty = not reached. */
    std::vector<uint8_t> active_;                  /**< Per tile; empty = no restriction (exact). */
    std::size_t active_count_ = 0;
    std::size_t coarse_count_ = 0;
    float coarse_scale_ = 1.0f;                    /**< Applied to coarse distances (wall penalty). */
    struct GoalSpan {
        SpanRef ref;
        glm::vec3 pos;
    };
    std::vector<GoalSpan> goal_spans_;

    friend class FlowFieldBuilder;
};

/**
 * @class FlowFieldBuilder
 * @brief A FlowField built in resumable pieces, so a background build never occupies a worker
 *        (or a thread that is merely waiting on frame work and helps out) for more than a slice.
 *
 * march() a slice at a time until it returns true, then directions() over [0, tile_count())
 * in any chunks on any threads, then finish(). See submit_flow_field_build() for the job chain
 * NavSystem uses.
 */
class FlowFieldBuilder {
public:
    FlowFieldBuilder(std::shared_ptr<const NavMesh> mesh, std::span<const glm::vec3> goals,
                     const FlowFieldSettings& settings = {})
        : field_(new FlowField()) {
        field_->mesh_ = std::move(mesh);
        field_->settings_ = settings;
        field_->goals_.assign(goals.begin(), goals.end());
        // No work here: construction happens on whichever thread kicks the build (the frame,
        // for NavSystem), so planning -- the coarse pass, the active set, the seeds -- is the
        // first march() slice's job instead.
    }

    /** @brief Marches up to `max_spans` more spans; true once marching is done. The first call
     *         also plans (coarse pass, active set, seeds). */
    bool march(std::size_t max_spans) {
        if (!started_) {
            started_ = true;
            if (!field_->mesh_) return finish_march_();
            field_->begin_march_();
            return false; // planning was this slice
        }
        if (!field_->march_) return true;
        if (!field_->march_step_(max_spans)) return false;
        return finish_march_();
    }

    /** @brief Tiles needing directions (valid after march() returned true). */
    std::size_t tile_count() const { return touched_.size(); }

    /** @brief Computes directions for touched tiles [begin, end). Thread-safe across disjoint ranges. */
    void directions(std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end && i < touched_.size(); ++i) field_->directions_for_tile_(touched_[i]);
    }

    std::shared_ptr<const FlowField> finish() { return field_; }

private:
    bool finish_march_() {
        if (field_->march_) field_->end_march_();
        touched_ = field_->touched_tiles_();
        return true;
    }

    std::shared_ptr<FlowField> field_;
    std::vector<uint32_t> touched_;
    bool started_ = false;
};

/**
 * @brief Schedules `builder` on `jobs` as a chain of short jobs, all contributing to `handle`:
 *        march slices of `slice` spans (each resubmits the next), then the direction pass split
 *        into chunks of `tiles_per_job` tiles. The build is done when `handle` completes; take the
 *        result with builder->finish().
 *
 * Why slices: a waiting thread (any wait_for(), e.g. a frame's parallel_for_blocking) runs
 * whatever job it can find, whatever its priority. A monolithic multi-millisecond build is a
 * frame hitch the moment a waiter picks it up; a slice costs it a fraction of a millisecond.
 */
inline void submit_flow_field_build(coopa::job::JobEngine& jobs, const coopa::job::JobHandle& handle,
                                    std::shared_ptr<FlowFieldBuilder> builder,
                                    coopa::job::Priority priority = coopa::job::Priority::Low,
                                    std::size_t slice = 4096, std::size_t tiles_per_job = 4) {
    struct Step {
        std::shared_ptr<FlowFieldBuilder> b;
        std::size_t slice;
        std::size_t tiles_per_job;
        coopa::job::Priority priority;
        void operator()(const coopa::job::JobContext& ctx) const {
            if (!b->march(slice)) {
                ctx.engine->submit(*this, 0, ctx.group, nullptr, 0u, priority);
                return;
            }
            const std::size_t n = b->tile_count();
            for (std::size_t begin = 0; begin < n; begin += tiles_per_job) {
                std::size_t end = std::min(begin + tiles_per_job, n);
                auto bb = b;
                ctx.engine->submit([bb, begin, end]() { bb->directions(begin, end); }, 0, ctx.group, nullptr, 0u, priority);
            }
        }
    };
    jobs.submit(Step{std::move(builder), std::max<std::size_t>(slice, 1), std::max<std::size_t>(tiles_per_job, 1), priority},
                0, handle, nullptr, 0u, priority);
}

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_FLOW_FIELD_H
