/**
 * @file crowd.h
 * @brief Crowd -- scene-agnostic movement for many agents on a NavMesh: path following, flow
 *        field following, separation and simple head-on avoidance, wall sliding and off-mesh
 *        link traversal, updated in parallel on the job engine.
 *
 * Every update is two-phase so it parallelizes without locks and stays deterministic:
 * positions and velocities are snapshotted, a spatial hash over the snapshot is built, then
 * each agent computes its new state reading only the snapshot and writing only its own slot.
 */

#ifndef PHYSXCOOPA_NAV_CROWD_H
#define PHYSXCOOPA_NAV_CROWD_H

#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_mesh.h>
#include <physxcoopa/nav/flow_field.h>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace physx {
namespace nav {

/** @brief Per-agent movement tunables. */
struct CrowdAgentParams {
    float radius = 0.4f;
    float height = 1.8f;
    float max_speed = 3.5f;
    float max_acceleration = 12.0f;
    /** @brief Stop when this close to the goal. */
    float stopping_distance = 0.15f;
    /** @brief Start slowing down this far from the goal (0 = arrive at full speed). */
    float slowdown_distance = 1.0f;
    /** @brief Push away from neighbours; 0 disables avoidance entirely. */
    float separation_weight = 2.0f;
    /** @brief Extra gap kept beyond the two radii. */
    float separation_margin = 0.2f;
    /** @brief Seconds per metre on an off-mesh link (drops/jumps/ladders). */
    float link_seconds_per_metre = 0.25f;
    QueryFilter filter;
};

enum class CrowdMode : uint8_t { Idle, Path, Flow };

/** @brief One agent's state. Read freely between updates; mutate through Crowd's setters. */
struct CrowdAgent {
    bool active = false;
    CrowdAgentParams params;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec3 desired_velocity{0.0f};
    SpanRef ref = k_invalid_span;

    CrowdMode mode = CrowdMode::Idle;
    NavPath path;
    uint32_t corner = 1;                 /**< Next path point to steer for. */
    std::shared_ptr<const FlowField> flow;
    bool arrived = false;
    /** @brief Distance left to the goal: flow-field distance, or along the path's corners. */
    float goal_distance = 0.0f;
    /** @brief Identifies the goal (its quantized position), so agents heading to the same place
     *         can tell -- see Crowd's arrival contagion. */
    uint64_t target_key = 0;
    /** @brief Flow mode: whether the field covered this agent at its last update. A bounded
     *         field (FlowFieldSettings::required_points) that an agent wandered out of reads
     *         false here, which tells NavSystem to rebuild it. */
    bool flow_valid = true;
    /** @brief Flow mode: the agent is outside its hierarchical field's exact tiles and steering
     *         by the coarse layer -- NavSystem rebuilds the field to cover it. */
    bool flow_coarse = false;

    bool on_link = false;
    glm::vec3 link_from{0.0f};
    glm::vec3 link_to{0.0f};
    float link_t = 0.0f;
    float link_duration = 0.0f;

    uint64_t user_data = 0;

    /** @brief Straight-line distance left along the current path (or flow distance). */
    float remaining_distance() const {
        if (mode == CrowdMode::Path && path.valid()) {
            float d = 0.0f;
            glm::vec3 p = position;
            for (std::size_t i = corner; i < path.points.size(); ++i) {
                d += glm::distance(p, path.points[i]);
                p = path.points[i];
            }
            return d;
        }
        return 0.0f;
    }
};

class Crowd {
public:
    using AgentId = uint32_t;
    static constexpr AgentId k_invalid_agent = 0xFFFFFFFFu;

    AgentId add_agent(const glm::vec3& position, const CrowdAgentParams& params) {
        AgentId id;
        if (!free_.empty()) {
            id = free_.back();
            free_.pop_back();
        } else {
            id = static_cast<AgentId>(agents_.size());
            agents_.emplace_back();
        }
        CrowdAgent& a = agents_[id];
        a = CrowdAgent{};
        a.active = true;
        a.params = params;
        a.position = position;
        return id;
    }

    void remove_agent(AgentId id) {
        if (id >= agents_.size() || !agents_[id].active) return;
        agents_[id] = CrowdAgent{};
        free_.push_back(id);
    }

    bool valid(AgentId id) const { return id < agents_.size() && agents_[id].active; }
    CrowdAgent& agent(AgentId id) { return agents_[id]; }
    const CrowdAgent& agent(AgentId id) const { return agents_[id]; }
    std::size_t capacity() const { return agents_.size(); }
    std::size_t active_count() const { return agents_.size() - free_.size(); }

    /** @brief Follow `path` (from find_path()). */
    void set_path(AgentId id, NavPath path) {
        CrowdAgent& a = agents_[id];
        a.path = std::move(path);
        a.mode = a.path.valid() ? CrowdMode::Path : CrowdMode::Idle;
        a.corner = 1;
        a.arrived = false;
        a.flow.reset();
    }

    /** @brief Follow a flow field; it can be swapped for a rebuilt one at any time. */
    void set_flow(AgentId id, std::shared_ptr<const FlowField> flow) {
        CrowdAgent& a = agents_[id];
        if (a.mode != CrowdMode::Flow || a.flow != flow) a.arrived = false;
        a.flow = std::move(flow);
        a.mode = a.flow ? CrowdMode::Flow : CrowdMode::Idle;
        a.path = NavPath{};
    }

    void stop(AgentId id) {
        CrowdAgent& a = agents_[id];
        a.mode = CrowdMode::Idle;
        a.path = NavPath{};
        a.flow.reset();
    }

    /** @brief Places an agent (e.g. after a teleport). */
    void warp(AgentId id, const glm::vec3& position) {
        CrowdAgent& a = agents_[id];
        a.position = position;
        a.velocity = glm::vec3(0.0f);
        a.ref = k_invalid_span;
        a.on_link = false;
    }

    /** @brief Advances every agent by `dt` on `mesh`. Parallel on `jobs` when given. */
    void update(float dt, const NavMesh& mesh, coopa::job::JobEngine* jobs, std::size_t parallel_threshold = 64) {
        if (dt <= 0.0f || agents_.empty()) return;
        const std::size_t n = agents_.size();
        const bool remap = mesh.version != mesh_version_ || &mesh != mesh_ptr_;
        mesh_version_ = mesh.version;
        mesh_ptr_ = &mesh;

        // Snapshot.
        snap_pos_.resize(n);
        snap_vel_.resize(n);
        snap_radius_.resize(n);
        snap_ref_.resize(n);
        snap_arrived_.resize(n);
        snap_goal_dist_.resize(n);
        snap_key_.resize(n);
        float max_reach = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            const CrowdAgent& a = agents_[i];
            snap_pos_[i] = a.position;
            snap_vel_[i] = a.velocity;
            snap_radius_[i] = a.active ? a.params.radius : -1.0f;
            snap_ref_[i] = remap ? k_invalid_span : a.ref;
            snap_arrived_[i] = a.active && a.mode != CrowdMode::Idle && a.arrived;
            snap_goal_dist_[i] = a.goal_distance;
            snap_key_[i] = a.target_key;
            if (a.active) max_reach = std::max(max_reach, 2.0f * a.params.radius + a.params.separation_margin);
        }
        build_hash_(std::max(max_reach, 0.1f));

        // Packing radius of every group of agents sharing a goal: the circle their bodies would
        // fill packed around it. Bounds arrival contagion (see step_agent_()).
        group_radius_.clear();
        std::unordered_map<uint64_t, float> area;
        for (std::size_t i = 0; i < n; ++i) {
            const CrowdAgent& a = agents_[i];
            if (!a.active || a.mode == CrowdMode::Idle || a.target_key == 0) continue;
            float d = 2.0f * a.params.radius + a.params.separation_margin;
            area[a.target_key] += 0.9f * d * d;
        }
        for (const auto& [key, ar] : area) group_radius_[key] = std::sqrt(ar / glm::pi<float>());

        auto run = [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                if (!agents_[i].active) continue;
                step_agent_(static_cast<AgentId>(i), dt, mesh, remap);
            }
        };
        // Frame-critical: the waiting thread helps only with Normal+ work, never a queued Low
        // background job (tile builds, flow slices, path queries) -- see JobEngine::wait_for().
        if (jobs && n >= parallel_threshold) {
            jobs->parallel_for_blocking(n, 64, run, 0, coopa::job::Priority::Normal, coopa::job::Priority::Normal);
        }
        else run(0, n);
    }

private:
    /** @brief A spatial hash over the snapshot: counting-sorted agent indices per bucket. */
    void build_hash_(float cell) {
        cell_ = cell;
        std::size_t buckets = 64;
        while (buckets < agents_.size() * 2) buckets <<= 1;
        bucket_start_.assign(buckets + 1, 0);
        bucket_items_.resize(agents_.size());
        auto key = [&](std::size_t i) { return bucket_of_(cell_index_(snap_pos_[i].x), cell_index_(snap_pos_[i].y)); };
        for (std::size_t i = 0; i < agents_.size(); ++i) {
            if (snap_radius_[i] >= 0.0f) ++bucket_start_[key(i) + 1];
        }
        for (std::size_t b = 0; b < buckets; ++b) bucket_start_[b + 1] += bucket_start_[b];
        std::vector<uint32_t>& fill = bucket_fill_;
        fill.assign(bucket_start_.begin(), bucket_start_.end() - 1);
        for (std::size_t i = 0; i < agents_.size(); ++i) {
            if (snap_radius_[i] >= 0.0f) bucket_items_[fill[key(i)]++] = static_cast<uint32_t>(i);
        }
    }

    int cell_index_(float v) const { return static_cast<int>(std::floor(v / cell_)); }
    std::size_t bucket_of_(int x, int y) const {
        uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u;
        return h & (bucket_start_.size() - 2);
    }

    static uint64_t goal_key_(const glm::vec3& g) {
        auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::floor(v * 4.0f)) & 0x1FFFFF); };
        return (q(g.x) << 42) | (q(g.y) << 21) | q(g.z) | 1ull << 63;
    }

    /**
     * @brief Whether agent `id` (at `pos` on `ref`) and neighbour `j` stand on connected surface
     *        -- so a crowd on the ground beside a staircase never pushes the agents climbing it
     *        through the stair's side. Pairs at the same height pass outright; others need a
     *        clear surface raycast from one to the other (a few cells: they are neighbours).
     */
    bool shares_surface_(const NavMesh& mesh, SpanRef ref, const glm::vec3& pos, uint32_t j, const QueryFilter& filter) const {
        float dz = std::fabs(snap_pos_[j].z - pos.z);
        if (dz < 0.1f) return true;
        if (dz > 2.0f * mesh.params.agent_height || ref == k_invalid_span || snap_ref_[j] == k_invalid_span) return false;
        NavRaycastHit hit;
        return mesh.raycast(ref, pos, snap_pos_[j], filter, hit) && hit.last == snap_ref_[j];
    }

    void begin_link_(CrowdAgent& a, const glm::vec3& from, const glm::vec3& to) {
        a.on_link = true;
        a.link_from = from;
        a.link_to = to;
        a.link_t = 0.0f;
        a.link_duration = std::max(0.05f, glm::distance(from, to) * a.params.link_seconds_per_metre);
    }

    /** @brief Advances one agent. Reads other agents only through the snapshot arrays and
     *         writes only agents_[id], which is what makes the parallel update race-free. */
    void step_agent_(AgentId id, float dt, const NavMesh& mesh, bool remap) {
        CrowdAgent& a = agents_[id];
        const CrowdAgentParams& p = a.params;

        if (a.on_link) {
            a.link_t += dt / a.link_duration;
            float t = std::min(a.link_t, 1.0f);
            float arc = 4.0f * t * (1.0f - t) * std::min(1.0f, 0.25f * glm::distance(a.link_from, a.link_to));
            a.position = glm::mix(a.link_from, a.link_to, t) + glm::vec3(0.0f, 0.0f, arc);
            a.velocity = (a.link_to - a.link_from) / a.link_duration;
            if (a.link_t >= 1.0f) {
                a.on_link = false;
                a.ref = mesh.locate(a.link_to);
                if (a.mode == CrowdMode::Path) ++a.corner;
            }
            return;
        }

        if (remap || a.ref == k_invalid_span || !mesh.span(a.ref)) {
            a.ref = mesh.locate(a.position);
            if (a.ref == k_invalid_span) a.ref = mesh.find_nearest(a.position, glm::vec3(2.0f, 2.0f, 4.0f), &p.filter);
        } else {
            // Cheap per-frame relocate: still in the same column -> keep, else re-snap.
            glm::ivec2 c = mesh.cell_of(a.ref);
            if (c != mesh.params.cell_of(a.position)) a.ref = mesh.locate(a.position);
        }
        if (a.ref == k_invalid_span) {
            a.velocity = glm::vec3(0.0f);
            return; // off the mesh: nothing sensible to do
        }

        // --- Desired velocity ---
        glm::vec2 desired(0.0f);
        const glm::vec2 pos2(a.position);
        if (a.mode == CrowdMode::Path && !a.arrived) {
            const auto& pts = a.path.points;
            while (a.corner < pts.size()) {
                glm::vec2 c(pts[a.corner]);
                bool last = a.corner + 1 == pts.size();
                float reach = last ? p.stopping_distance : std::max(0.5f * p.radius, 0.1f);
                bool close_z = std::fabs(pts[a.corner].z - a.position.z) < std::max(p.height * 0.5f, 1.0f);
                if (glm::distance(c, pos2) > reach || !close_z) break;
                if (a.path.flags[a.corner] & k_path_point_link_start && a.corner + 1 < pts.size()) {
                    begin_link_(a, a.position, pts[a.corner + 1]);
                    ++a.corner;
                    return;
                }
                if (last) {
                    a.arrived = true;
                    break;
                }
                ++a.corner;
            }
            a.target_key = goal_key_(pts.back());
            a.goal_distance = a.remaining_distance();
            if (!a.arrived && a.corner < pts.size()) {
                glm::vec2 to = glm::vec2(pts[a.corner]) - pos2;
                float dist = glm::length(to);
                float speed = p.max_speed;
                if (a.corner + 1 == pts.size() && p.slowdown_distance > 0.0f) {
                    speed *= std::clamp(dist / p.slowdown_distance, 0.15f, 1.0f);
                }
                if (dist > 1e-5f) desired = to / dist * speed;
            }
        } else if (a.mode == CrowdMode::Flow && a.flow) {
            // Same mesh as the field (the common case): sample from the span we already know
            // instead of locating it again.
            FlowSample s = &a.flow->mesh() == &mesh ? a.flow->sample(a.ref, a.position) : a.flow->sample(a.position);
            a.flow_valid = s.valid;
            a.flow_coarse = s.coarse;
            a.target_key = a.flow->goals().empty() ? 0 : goal_key_(a.flow->goals()[0]);
            a.goal_distance = s.valid ? s.distance : std::numeric_limits<float>::max();
            if (s.valid) {
                if (s.link >= 0) {
                    const OffMeshLink& l = a.flow->mesh().links[s.link];
                    bool reverse = s.ref != l.start_ref;
                    glm::vec3 from = reverse ? l.end : l.start;
                    glm::vec3 to = reverse ? l.start : l.end;
                    if (glm::distance(glm::vec2(from), pos2) < std::max(p.radius, a.flow->mesh().params.cs)) {
                        begin_link_(a, a.position, to);
                        return;
                    }
                    glm::vec2 v = glm::vec2(from) - pos2;
                    float l2 = glm::length(v);
                    desired = l2 > 1e-5f ? v / l2 * p.max_speed : glm::vec2(0.0f);
                } else {
                    // A crowd can't all stand on the goal point: arrive within half a radius, and
                    // stay arrived until pushed a radius further out (hysteresis, so jostling
                    // doesn't flip an agent back to pressing in). Neighbours further out arrive by
                    // contagion (see the separation pass).
                    float arrive = std::max(p.stopping_distance, 0.5f * p.radius);
                    a.arrived = s.distance <= (a.arrived ? arrive + p.radius : arrive);
                    float speed = p.max_speed;
                    if (p.slowdown_distance > 0.0f) speed *= std::clamp(s.distance / p.slowdown_distance, 0.1f, 1.0f);
                    desired = a.arrived ? glm::vec2(0.0f) : glm::vec2(s.direction) * speed;
                }
            }
        }

        static constexpr int k_max_candidates = 32;
        uint32_t candidates[k_max_candidates];
        int ncand = 0;

        // --- Separation and keep-right avoidance ---
        // The push is AVERAGED over neighbours, not summed: summed, a dense crowd's push grows
        // with its size and overwhelms the steering, pinning agents against walls. Neighbours
        // ahead (along the direction of travel) count fully, those behind a quarter.
        if (p.separation_weight > 0.0f) {
            glm::vec2 push(0.0f);
            float desired_speed = glm::length(desired);
            glm::vec2 ddir = desired_speed > 1e-4f ? desired / desired_speed : glm::vec2(0.0f);
            int npush = 0;
            bool contagion = false;
            ncand = 0;
            float group_radius = 0.0f;
            if (auto g = group_radius_.find(a.target_key); g != group_radius_.end()) group_radius = g->second;
            int cx = cell_index_(pos2.x), cy = cell_index_(pos2.y);
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    std::size_t b = bucket_of_(cx + ox, cy + oy);
                    for (uint32_t k = bucket_start_[b]; k < bucket_start_[b + 1]; ++k) {
                        uint32_t j = bucket_items_[k];
                        if (j == id) continue;
                        glm::vec3 pj = snap_pos_[j];
                        glm::vec2 d = pos2 - glm::vec2(pj);
                        float contact = p.radius + snap_radius_[j];
                        float range = contact + p.separation_margin;
                        float dist2 = glm::dot(d, d);
                        // Cheap distance reject before the surface check (which may raycast).
                        float reach = range + p.max_speed * dt;
                        if (dist2 >= reach * reach) continue;
                        if (!shares_surface_(mesh, a.ref, a.position, j, p.filter)) continue;
                        // Remembered for the overlap pass below, which would otherwise repeat
                        // this whole scan.
                        if (ncand < k_max_candidates) candidates[ncand++] = j;
                        if (dist2 >= range * range) continue;
                        float dist = std::sqrt(dist2);
                        glm::vec2 away = dist > 1e-4f ? d / dist : glm::vec2(static_cast<float>((id * 7 + j) % 3) - 1.0f, 0.5f);
                        float w = (range - dist) / range;
                        float ahead = glm::dot(-away, ddir); // +1: straight in front
                        // Arrival contagion: touching an agent that already arrived at the same
                        // goal, nearer to it than we are, means we have arrived too -- within the
                        // group's packing radius. Without it everyone presses toward one point
                        // and swirls there; unbounded, it spreads back down the queue and stops
                        // agents that still had room to move in.
                        if (snap_arrived_[j] && a.mode != CrowdMode::Idle && snap_key_[j] == a.target_key &&
                            snap_goal_dist_[j] < a.goal_distance && dist < contact + p.separation_margin &&
                            a.goal_distance <= group_radius) {
                            contagion = true;
                        }
                        push += away * (w * (ahead > 0.0f ? 1.0f : 0.25f));
                        ++npush;
                        glm::vec2 vj(snap_vel_[j]);
                        // Oncoming neighbour straight ahead: both step to their right.
                        if (ahead > 0.7f && glm::dot(vj, ddir) < 0.0f) {
                            push += glm::vec2(ddir.y, -ddir.x) * (0.5f * w);
                        }
                    }
                }
            }
            if (npush > 0) push /= static_cast<float>(npush);
            if (contagion) {
                a.arrived = true;
                desired = glm::vec2(0.0f);
            }
            desired += push * (p.separation_weight * p.max_speed);
            float l = glm::length(desired);
            if (l > p.max_speed) desired *= p.max_speed / l;
        }
        a.desired_velocity = glm::vec3(desired, 0.0f);

        // --- Integrate ---
        glm::vec2 v(a.velocity);
        glm::vec2 dv = desired - v;
        float max_dv = p.max_acceleration * dt;
        float ldv = glm::length(dv);
        if (ldv > max_dv) dv *= max_dv / ldv;
        v += dv;
        glm::vec3 target = a.position + glm::vec3(v * dt, 0.0f);
        NavMoveResult mv = mesh.move_along_surface(a.ref, a.position, target, p.filter);
        if (mv.blocked) v = (glm::vec2(mv.position) - pos2) / dt; // lose the into-wall component

        // --- Overlap resolve ---
        // Steering is soft: a crowd pressing into a doorway still compresses. Push out of half of
        // each remaining overlap (the neighbour, doing the same, takes the other half), against
        // the snapshot so the parallel update stays race-free, and along the surface so the push
        // never leaves the mesh.
        if (p.separation_weight > 0.0f) {
            glm::vec2 np(mv.position);
            glm::vec2 corr(0.0f);
            for (int c = 0; c < ncand; ++c) {
                uint32_t j = candidates[c];
                glm::vec2 d = np - glm::vec2(snap_pos_[j]);
                float range = p.radius + snap_radius_[j];
                float dist2 = glm::dot(d, d);
                if (dist2 >= range * range || dist2 < 1e-10f) continue;
                float dist = std::sqrt(dist2);
                corr += d / dist * (0.5f * (range - dist));
            }
            float lc = glm::length(corr);
            if (lc > 1e-5f) {
                corr *= std::min(1.0f, 0.5f * p.radius / lc);
                NavMoveResult pushed = mesh.move_along_surface(mv.ref, mv.position, mv.position + glm::vec3(corr, 0.0f), p.filter);
                mv.position = pushed.position;
                mv.ref = pushed.ref;
            }
        }
        float vz = (mv.position.z - a.position.z) / dt;
        a.position = mv.position;
        a.ref = mv.ref;
        a.velocity = glm::vec3(v, vz);
    }

    std::vector<CrowdAgent> agents_;
    std::vector<AgentId> free_;
    uint64_t mesh_version_ = ~0ull;
    const NavMesh* mesh_ptr_ = nullptr;

    std::vector<glm::vec3> snap_pos_;
    std::vector<glm::vec3> snap_vel_;
    std::vector<float> snap_radius_;
    std::vector<SpanRef> snap_ref_;
    std::vector<uint8_t> snap_arrived_;
    std::vector<float> snap_goal_dist_;
    std::vector<uint64_t> snap_key_;
    std::unordered_map<uint64_t, float> group_radius_;
    std::vector<uint32_t> bucket_start_;
    std::vector<uint32_t> bucket_fill_;
    std::vector<uint32_t> bucket_items_;
    float cell_ = 1.0f;
};

} // namespace nav
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_NAV_CROWD_H
