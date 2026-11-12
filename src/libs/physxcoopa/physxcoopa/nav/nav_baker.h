/**
 * @file nav_baker.h
 * @brief NavBaker -- owns the build inputs (sources, volumes, links) and the current NavMesh of
 *        every agent profile, and keeps them in sync: a blocking parallel full build, then
 *        incremental tile rebuilds on the job engine as the inputs change.
 *
 * Change detection is by diff: set_sources() compares each source's key and fingerprint with
 * the previous set, and every tile whose padded footprint touches an added, removed or changed
 * source is marked dirty. A dirty tile waits until it has been quiet for
 * NavBuildSettings::rebuild_delay (or dirty for four times that), then its triangles are
 * gathered on the calling thread -- the one place that reads mesh assets -- and the rasterize
 * + per-agent build runs as a Priority::Low job. Finished tiles are committed in a batch on a
 * later update(), producing a new NavMesh per agent (see NavMesh::commit()).
 */

#ifndef PHYSXCOOPA_NAV_NAV_BAKER_H
#define PHYSXCOOPA_NAV_NAV_BAKER_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_source.h>
#include <physxcoopa/nav/heightfield.h>
#include <physxcoopa/nav/nav_tile.h>
#include <physxcoopa/nav/nav_mesh.h>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/** @brief Counters for diagnostics and tests. */
struct NavBuildStats {
    uint32_t full_builds = 0;
    uint32_t tiles_built = 0;        /**< Total tile builds (all agents count once). */
    uint32_t commits = 0;
    double last_full_build_ms = 0.0;
    std::size_t total_spans = 0;     /**< Agent 0's span count after the last commit. */
};

class NavBaker {
public:
    explicit NavBaker(NavBuildSettings settings = {}) : settings_(std::move(settings)) {
        if (settings_.agents.empty()) settings_.agents.push_back(AgentProfile{});
    }

    const NavBuildSettings& settings() const { return settings_; }

    /** @brief Replaces the settings; everything rebuilds on the next update()/build_all(). */
    void set_settings(NavBuildSettings settings) {
        wait_pending_(nullptr); // running builds read settings_
        settings_ = std::move(settings);
        if (settings_.agents.empty()) settings_.agents.push_back(AgentProfile{});
        needs_full_build_ = true;
    }

    /**
     * @brief Changes only how incremental rebuilds are PACED, without re-arming a full build.
     *
     * `rebuild_delay` and `max_concurrent_rebuilds` decide when a dirty tile is picked up and how
     * many go at once; neither changes the geometry a tile bakes to, so unlike set_settings()
     * this needs no rebuild. For a host that wants the navmesh finished as fast as it can be
     * rather than spread over gameplay frames -- a loading screen holding a scene until its world
     * is built -- and then put back.
     *
     * @param rebuild_delay Seconds a dirty tile waits before it is rebuilt; 0 rebuilds at once.
     * @param max_concurrent_rebuilds Tiles that may be rebuilding together; <= 0 leaves it.
     */
    void set_rebuild_pacing(float rebuild_delay, int max_concurrent_rebuilds) {
        settings_.rebuild_delay = std::max(0.0f, rebuild_delay);
        if (max_concurrent_rebuilds > 0) settings_.max_concurrent_rebuilds = max_concurrent_rebuilds;
    }

    // --- Inputs -----------------------------------------------------------------------------

    /** @brief Replaces the source set, dirtying every tile an added, removed or changed source touches. */
    void set_sources(std::vector<SourceShape> sources) {
        std::unordered_map<uint64_t, std::size_t> old_by_key;
        old_by_key.reserve(sources_.size());
        for (std::size_t i = 0; i < sources_.size(); ++i) old_by_key[sources_[i].key] = i;
        std::vector<bool> old_seen(sources_.size(), false);
        bool any = sources.size() != sources_.size();
        for (const SourceShape& s : sources) {
            auto it = old_by_key.find(s.key);
            if (it == old_by_key.end()) {
                dirty_bounds_(s.bounds);
                any = true;
                continue;
            }
            old_seen[it->second] = true;
            if (sources_[it->second].fingerprint != s.fingerprint) {
                dirty_bounds_(sources_[it->second].bounds);
                dirty_bounds_(s.bounds);
                any = true;
            }
        }
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            if (!old_seen[i]) {
                dirty_bounds_(sources_[i].bounds);
                any = true;
            }
        }
        if (!any) return;
        if (settings_.bounds.min.x > settings_.bounds.max.x && built_) {
            // Auto-fitted bounds: a source outside the grid needs a bigger grid.
            for (const SourceShape& s : sources) {
                if (s.bounds.min.x < grid_bounds_.min.x || s.bounds.min.y < grid_bounds_.min.y ||
                    s.bounds.max.x > grid_bounds_.max.x || s.bounds.max.y > grid_bounds_.max.y ||
                    s.bounds.min.z < grid_bounds_.min.z) {
                    needs_full_build_ = true;
                    break;
                }
            }
        }
        sources_ = std::move(sources);
        tile_sources_dirty_ = true;
    }

    /** @brief Replaces the area volumes, dirtying the tiles of every added/removed/changed one. */
    void set_volumes(std::vector<SourceVolume> volumes) {
        auto key_set = [](const std::vector<SourceVolume>& v) {
            std::unordered_set<uint64_t> s;
            for (const auto& x : v) s.insert(x.fingerprint);
            return s;
        };
        auto old_set = key_set(volumes_);
        auto new_set = key_set(volumes);
        bool any = false;
        for (const auto& v : volumes_) {
            if (!new_set.count(v.fingerprint)) { dirty_bounds_(v.bounds()); any = true; }
        }
        for (const auto& v : volumes) {
            if (!old_set.count(v.fingerprint)) { dirty_bounds_(v.bounds()); any = true; }
        }
        if (any || volumes.size() != volumes_.size()) volumes_ = std::move(volumes);
    }

    /** @brief Replaces the off-mesh link set (applied at the next commit, no tile rebuild). */
    void set_links(std::vector<OffMeshLink> links) {
        if (links.size() == links_.size()) {
            bool same = true;
            for (std::size_t i = 0; i < links.size() && same; ++i) {
                const auto& a = links[i];
                const auto& b = links_[i];
                same = a.start == b.start && a.end == b.end && a.bidirectional == b.bidirectional &&
                       a.cost == b.cost && a.area == b.area && a.user_id == b.user_id && a.snap_radius == b.snap_radius;
            }
            if (same) return;
        }
        links_ = std::move(links);
        links_dirty_ = true;
    }

    const std::vector<SourceShape>& sources() const { return sources_; }

    /** @brief Marks every tile overlapping `bounds` dirty (e.g. after editing a mesh asset in place). */
    void mark_dirty(const geometry::AABB& bounds) { dirty_bounds_(bounds); }

    // --- Building ---------------------------------------------------------------------------

    /**
     * @brief Blocking full build of every agent's mesh, tiles in parallel on `jobs` (inline
     *        when null). Fits the grid to the sources when settings().bounds is empty.
     */
    void build_all(coopa::job::JobEngine* jobs) {
        auto t0 = std::chrono::steady_clock::now();
        wait_pending_(jobs);
        for (auto& p : pending_) p->handle.close(); // superseded by this full build
        pending_.clear();
        in_flight_.clear();
        dirty_.clear();
        compute_grid_();
        rebuild_tile_sources_();

        const uint32_t tile_count = base_params_.tile_count();
        const std::size_t agent_count = settings_.agents.size();
        std::vector<std::vector<std::shared_ptr<NavTile>>> results(agent_count, std::vector<std::shared_ptr<NavTile>>(tile_count));
        auto build_range = [&](std::size_t begin, std::size_t end) {
            TileInput input;
            for (std::size_t i = begin; i < end; ++i) {
                gather_tile_input_(static_cast<uint32_t>(i), input);
                auto tiles = build_tile_input_(input);
                for (std::size_t a = 0; a < agent_count; ++a) results[a][i] = std::move(tiles[a]);
            }
        };
        if (jobs && tile_count > 1) {
            jobs->parallel_for_blocking(tile_count, 1, build_range);
        } else {
            build_range(0, tile_count);
        }
        stats_.tiles_built += tile_count;

        meshes_.assign(agent_count, nullptr);
        for (std::size_t a = 0; a < agent_count; ++a) {
            auto empty = NavMesh::make_empty(agent_params_(a), settings_.areas);
            std::vector<std::pair<uint32_t, std::shared_ptr<NavTile>>> replaced;
            replaced.reserve(tile_count);
            for (uint32_t i = 0; i < tile_count; ++i) {
                if (results[a][i]) replaced.emplace_back(i, std::move(results[a][i]));
            }
            meshes_[a] = NavMesh::commit(*empty, std::move(replaced), links_);
        }
        links_dirty_ = false;
        needs_full_build_ = false;
        built_ = true;
        ++stats_.full_builds;
        ++stats_.commits;
        update_span_stat_();
        stats_.last_full_build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    /**
     * @brief Incremental step: commits finished background tile builds, then starts builds for
     *        dirty tiles that are due. Call once per frame. Without a job engine the due tiles
     *        build and commit inline.
     */
    void update(float dt, coopa::job::JobEngine* jobs) {
        time_ += static_cast<double>(dt);
        if (!built_ || needs_full_build_) {
            build_all(jobs);
            return;
        }
        if (tile_sources_dirty_) rebuild_tile_sources_();

        // 1. Collect finished builds.
        std::vector<std::vector<std::pair<uint32_t, std::shared_ptr<NavTile>>>> replaced(settings_.agents.size());
        bool any_done = false;
        for (std::size_t i = 0; i < pending_.size();) {
            Pending& p = *pending_[i];
            if (p.handle.is_valid() && !p.handle.is_complete()) {
                ++i;
                continue;
            }
            p.handle.close();
            for (std::size_t a = 0; a < p.output.size() && a < replaced.size(); ++a) {
                replaced[a].emplace_back(p.tile, std::move(p.output[a]));
            }
            in_flight_.erase(p.tile);
            any_done = true;
            pending_[i] = std::move(pending_.back());
            pending_.pop_back();
        }
        if (any_done || links_dirty_) {
            for (std::size_t a = 0; a < meshes_.size(); ++a) {
                meshes_[a] = NavMesh::commit(*meshes_[a], std::move(replaced[a]), links_);
            }
            links_dirty_ = false;
            ++stats_.commits;
            update_span_stat_();
        }

        // 2. Kick due dirty tiles.
        const double delay = std::max(0.0f, settings_.rebuild_delay);
        const double max_wait = std::max(1.0, 4.0 * delay);
        std::vector<uint32_t> due;
        for (auto& [tile, d] : dirty_) {
            if (in_flight_.count(tile)) continue;
            if (time_ - d.last >= delay || time_ - d.first >= max_wait) due.push_back(tile);
        }
        std::sort(due.begin(), due.end());
        std::size_t budget = static_cast<std::size_t>(std::max(1, settings_.max_concurrent_rebuilds));
        budget = budget > pending_.size() ? budget - pending_.size() : 0;
        if (due.size() > budget) due.resize(budget);
        for (uint32_t tile : due) {
            dirty_.erase(tile);
            auto p = std::make_unique<Pending>();
            p->tile = tile;
            gather_tile_input_(tile, p->input);
            if (jobs) {
                p->handle = jobs->create_handle();
                Pending* raw = p.get();
                jobs->submit([this, raw]() { raw->output = build_tile_input_(raw->input); },
                             0, p->handle, nullptr, 0u, coopa::job::Priority::Low);
            } else {
                p->output = build_tile_input_(p->input);
            }
            in_flight_.insert(tile);
            pending_.push_back(std::move(p));
            ++stats_.tiles_built;
        }
        if (!jobs && !due.empty()) update(0.0f, nullptr); // commit the inline builds now
    }

    /** @brief Whether tile builds are queued or running. */
    bool busy() const { return !dirty_.empty() || !pending_.empty() || needs_full_build_; }

    /** @brief Blocks until every queued and running build is committed (tests, loading screens). */
    void flush(coopa::job::JobEngine* jobs) {
        for (int guard = 0; guard < 100000 && busy(); ++guard) {
            wait_pending_(jobs);
            update(std::max(1.0f, settings_.rebuild_delay * 8.0f), jobs);
        }
    }

    std::shared_ptr<const NavMesh> mesh(uint32_t agent = 0) const {
        return agent < meshes_.size() ? meshes_[agent] : nullptr;
    }
    std::size_t agent_count() const { return settings_.agents.size(); }
    bool built() const { return built_; }
    const NavBuildStats& stats() const { return stats_; }
    std::size_t dirty_tile_count() const { return dirty_.size() + pending_.size(); }

    ~NavBaker() {
        wait_pending_(nullptr);
        for (auto& p : pending_) p->handle.close();
    }

    NavBaker(const NavBaker&) = delete;
    NavBaker& operator=(const NavBaker&) = delete;

private:
    struct TileInput {
        uint32_t tile = 0;
        uint32_t revision = 0;
        std::vector<glm::vec3> verts;
        std::vector<uint8_t> tri_areas;
        std::vector<SourceVolume> volumes;
    };

    struct Pending {
        uint32_t tile = 0;
        TileInput input;
        std::vector<std::shared_ptr<NavTile>> output;
        coopa::job::JobHandle handle;
    };

    struct DirtyTimes {
        double first = 0.0;
        double last = 0.0;
    };

    void wait_pending_(coopa::job::JobEngine* jobs) {
        for (auto& p : pending_) {
            if (!p->handle.is_valid()) continue;
            if (jobs) {
                jobs->wait_for(p->handle);
            } else {
                while (!p->handle.is_complete()) std::this_thread::yield();
            }
        }
    }

    void compute_grid_() {
        geometry::AABB b = settings_.bounds;
        if (b.min.x > b.max.x) {
            b = geometry::AABB{};
            for (const auto& s : sources_) b = geometry::AABB::merge(b, s.bounds);
            if (b.min.x > b.max.x) {
                b.min = glm::vec3(-1.0f);
                b.max = glm::vec3(1.0f);
            }
            b = b.expand(settings_.bounds_padding);
        }
        grid_bounds_ = b;
        NavGridParams p;
        p.cs = std::max(settings_.cell_size, 0.01f);
        p.ch = std::max(settings_.cell_height, 0.005f);
        p.tile_size = std::clamp(settings_.tile_size, 4, 128);
        // Snapped to whole cells and quanta, so round-number heights (a floor at z = 0, a step
        // every 0.25 m) quantize exactly instead of reading a fraction of a quantum high.
        p.origin = glm::vec3(std::floor(b.min.x / p.cs) * p.cs, std::floor(b.min.y / p.cs) * p.cs,
                             std::floor((b.min.z - 1.0f) / p.ch) * p.ch);
        float tile_w = static_cast<float>(p.tile_size) * p.cs;
        p.tiles_x = std::max(1, static_cast<int>(std::ceil((b.max.x - b.min.x) / tile_w)));
        p.tiles_y = std::max(1, static_cast<int>(std::ceil((b.max.y - b.min.y) / tile_w)));
        while (static_cast<uint64_t>(p.tiles_x) * static_cast<uint64_t>(p.tiles_y) > 0xFFFEull) {
            // SpanRef reserves 16 bits for the tile: grow the tiles instead of overflowing.
            p.tile_size = std::min(128, p.tile_size * 2);
            tile_w = static_cast<float>(p.tile_size) * p.cs;
            p.tiles_x = std::max(1, static_cast<int>(std::ceil((b.max.x - b.min.x) / tile_w)));
            p.tiles_y = std::max(1, static_cast<int>(std::ceil((b.max.y - b.min.y) / tile_w)));
            if (p.tile_size == 128) break;
        }
        int max_radius = 0;
        int min_climb = 1 << 30;
        for (const auto& a : settings_.agents) {
            max_radius = std::max(max_radius, static_cast<int>(std::ceil(a.radius / p.cs)));
            min_climb = std::min(min_climb, static_cast<int>(std::floor(a.max_climb / p.ch)));
        }
        p.border = max_radius + 3;
        merge_threshold_ = std::max(0, min_climb);
        base_params_ = p;
    }

    NavGridParams agent_params_(std::size_t a) const {
        NavGridParams p = base_params_;
        const AgentProfile& ag = settings_.agents[a];
        p.walkable_height = std::max(1, static_cast<int>(std::ceil(ag.height / p.ch)));
        p.walkable_climb = std::max(0, static_cast<int>(std::floor(ag.max_climb / p.ch)));
        p.walkable_radius = std::max(0, static_cast<int>(std::ceil(ag.radius / p.cs)));
        p.agent_radius = ag.radius;
        p.agent_height = ag.height;
        return p;
    }

    /** @brief Buckets source indices by the tiles their padded footprint overlaps. */
    void rebuild_tile_sources_() {
        tile_sources_dirty_ = false;
        if (!built_ && base_params_.tiles_x == 0) return;
        tile_sources_.assign(base_params_.tile_count(), {});
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            glm::ivec2 t0, t1;
            if (!tile_range_(sources_[i].bounds, t0, t1)) continue;
            for (int ty = t0.y; ty <= t1.y; ++ty) {
                for (int tx = t0.x; tx <= t1.x; ++tx) {
                    tile_sources_[static_cast<std::size_t>(tx + ty * base_params_.tiles_x)].push_back(static_cast<uint32_t>(i));
                }
            }
        }
    }

    /** @brief Tiles whose padded footprint overlaps `b`, clamped to the grid. */
    bool tile_range_(const geometry::AABB& b, glm::ivec2& t0, glm::ivec2& t1) const {
        const NavGridParams& p = base_params_;
        if (p.tiles_x == 0) return false;
        float tile_w = static_cast<float>(p.tile_size) * p.cs;
        float pad = static_cast<float>(p.border) * p.cs;
        t0.x = static_cast<int>(std::floor((b.min.x - pad - p.origin.x) / tile_w));
        t0.y = static_cast<int>(std::floor((b.min.y - pad - p.origin.y) / tile_w));
        t1.x = static_cast<int>(std::floor((b.max.x + pad - p.origin.x) / tile_w));
        t1.y = static_cast<int>(std::floor((b.max.y + pad - p.origin.y) / tile_w));
        if (t1.x < 0 || t1.y < 0 || t0.x >= p.tiles_x || t0.y >= p.tiles_y) return false;
        t0 = glm::max(t0, glm::ivec2(0));
        t1 = glm::min(t1, glm::ivec2(p.tiles_x - 1, p.tiles_y - 1));
        return true;
    }

    void dirty_bounds_(const geometry::AABB& b) {
        if (!built_) return; // the first build covers everything anyway
        glm::ivec2 t0, t1;
        if (!tile_range_(b, t0, t1)) return;
        for (int ty = t0.y; ty <= t1.y; ++ty) {
            for (int tx = t0.x; tx <= t1.x; ++tx) {
                uint32_t i = static_cast<uint32_t>(tx + ty * base_params_.tiles_x);
                auto it = dirty_.find(i);
                if (it == dirty_.end()) dirty_[i] = DirtyTimes{time_, time_};
                else it->second.last = time_;
            }
        }
    }

    /** @brief Calling-thread half of a tile build: everything that reads sources or assets. */
    void gather_tile_input_(uint32_t tile, TileInput& in) {
        const NavGridParams& p = base_params_;
        int tx = static_cast<int>(tile % static_cast<uint32_t>(p.tiles_x));
        int ty = static_cast<int>(tile / static_cast<uint32_t>(p.tiles_x));
        in.tile = tile;
        in.revision = next_revision_.fetch_add(1, std::memory_order_relaxed);
        in.verts.clear();
        in.tri_areas.clear();
        in.volumes.clear();
        geometry::AABB clip = p.tile_bounds(tx, ty, p.border);
        if (tile < tile_sources_.size()) {
            for (uint32_t si : tile_sources_[tile]) {
                const SourceShape& s = sources_[si];
                std::size_t before = in.verts.size();
                emit_triangles(s, clip, in.verts);
                in.tri_areas.insert(in.tri_areas.end(), (in.verts.size() - before) / 3, s.area);
            }
        }
        for (const auto& v : volumes_) {
            geometry::AABB vb = v.bounds();
            if (vb.max.x >= clip.min.x && vb.min.x <= clip.max.x && vb.max.y >= clip.min.y && vb.min.y <= clip.max.y) {
                in.volumes.push_back(v);
            }
        }
    }

    /** @brief Job half of a tile build: rasterize once, build every agent's tile. Reads only
     *         `in` and immutable settings, so it is safe on any thread. */
    std::vector<std::shared_ptr<NavTile>> build_tile_input_(const TileInput& in) const {
        thread_local Heightfield hf;
        thread_local TileBuildScratch scratch;
        const NavGridParams& p = base_params_;
        std::vector<std::shared_ptr<NavTile>> out(settings_.agents.size());
        if (in.verts.empty()) return out;
        int tx = static_cast<int>(in.tile % static_cast<uint32_t>(p.tiles_x));
        int ty = static_cast<int>(in.tile / static_cast<uint32_t>(p.tiles_x));
        int dim = p.tile_size + 2 * p.border;
        glm::vec3 bmin(p.origin.x + static_cast<float>(tx * p.tile_size - p.border) * p.cs,
                       p.origin.y + static_cast<float>(ty * p.tile_size - p.border) * p.cs, p.origin.z);
        hf.reset(dim, dim, bmin, p.cs, p.ch);
        float walkable_cos = std::cos(glm::radians(std::clamp(settings_.max_slope_degrees, 0.0f, 89.9f)));
        hf.rasterize_triangles(in.verts, in.tri_areas, walkable_cos, merge_threshold_);
        for (std::size_t a = 0; a < out.size(); ++a) {
            auto tile = std::make_shared<NavTile>();
            build_tile(hf, agent_params_(a), tx, ty, in.volumes, in.revision, *tile, scratch);
            if (!tile->spans.empty()) out[a] = std::move(tile);
        }
        return out;
    }

    void update_span_stat_() {
        stats_.total_spans = 0;
        if (meshes_.empty() || !meshes_[0]) return;
        for (const auto& t : meshes_[0]->tiles) stats_.total_spans += t ? t->spans.size() : 0;
    }

    NavBuildSettings settings_;
    NavGridParams base_params_{};
    geometry::AABB grid_bounds_{};
    int merge_threshold_ = 1;

    std::vector<SourceShape> sources_;
    std::vector<SourceVolume> volumes_;
    std::vector<OffMeshLink> links_;
    std::vector<std::vector<uint32_t>> tile_sources_;
    bool tile_sources_dirty_ = true;
    bool links_dirty_ = false;
    bool needs_full_build_ = true;
    bool built_ = false;

    std::vector<std::shared_ptr<const NavMesh>> meshes_;
    std::unordered_map<uint32_t, DirtyTimes> dirty_;
    std::unordered_set<uint32_t> in_flight_;
    std::vector<std::unique_ptr<Pending>> pending_;
    double time_ = 0.0;
    std::atomic<uint32_t> next_revision_{1};
    NavBuildStats stats_;
};

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_NAV_BAKER_H
