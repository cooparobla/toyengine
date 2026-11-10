/**
 * @file nav_system.h
 * @brief NavSystem -- the ISceneSystem that builds navigation from a scene's colliders and
 *        moves its NavAgents. Reads the PhysicsSystem's world, so it must run after physics
 *        (default order 150: after Physics at 100, before the Behaviour walk at 200, so gameplay
 *        sees this frame's agent positions and its commands apply next frame).
 *
 * Per frame:
 *   1. gather: every contributing collider is snapshotted from the PhysicsWorld (NavModifier
 *      rules applied: area, unwalkable, ignore, carve-while-asleep for dynamic bodies), plus
 *      NavVolumes and NavLinks; NavBaker diffs them and rebuilds only the tiles that changed,
 *      in the background. The very first frame builds everything, blocking, in parallel.
 *   2. agents: NavAgent components are bound to a Crowd per agent profile; commands
 *      (set_destination & co.), teleports and parameter edits are picked up.
 *   3. flow fields: one per (target object, agent profile), shared by every agent following
 *      that target, rebuilt as a background job when the target moves or the mesh changes --
 *      agents keep following the previous field until the new one lands.
 *   4. paths: queued A* requests run in parallel on the job engine, at most
 *      `max_path_requests_per_frame` per frame; chasers re-plan as their target moves, and
 *      every path agent re-plans when its mesh changes.
 *   5. crowds step (parallel), and positions/rotations are written back to the Transforms.
 */

#ifndef PHYSXCOOPA_SYSTEM_NAV_SYSTEM_H
#define PHYSXCOOPA_SYSTEM_NAV_SYSTEM_H

#include <physxcoopa/system/physics_system.h>
#include <physxcoopa/components/collider.h>
#include <physxcoopa/components/nav_agent.h>
#include <physxcoopa/components/nav_modifier.h>
#include <physxcoopa/components/nav_volume.h>
#include <physxcoopa/components/nav_link.h>
#include <physxcoopa/nav/nav_types.h>
#include <physxcoopa/nav/nav_source.h>
#include <physxcoopa/nav/nav_baker.h>
#include <physxcoopa/nav/nav_mesh.h>
#include <physxcoopa/nav/path_query.h>
#include <physxcoopa/nav/flow_field.h>
#include <physxcoopa/nav/crowd.h>
#include <physxcoopa/nav/nav_debug.h>
#include <physxcoopa/nav/nav_settings.h>
#include <physxcoopa/util/transform_bridge.h>
#include <physxcoopa/debug/debug_draw.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coopa {
namespace physx {
namespace system {

/** @brief Per-stage main-thread cost of the last NavSystem::execute(), milliseconds. */
struct NavFrameStats {
    double gather_ms = 0.0;      /**< Collider/volume/link snapshot + diff. */
    double build_ms = 0.0;       /**< NavBaker::update(): commits, kicks (or a blocking full build). */
    double agents_ms = 0.0;      /**< NavAgent bind/command sync. */
    double flow_ms = 0.0;        /**< Flow-field bookkeeping (+ any blocking first build). */
    double paths_ms = 0.0;       /**< This frame's A* batch. */
    double crowd_ms = 0.0;       /**< Crowd steering + movement. */
    double write_back_ms = 0.0;  /**< Transforms + component state. */
    double total_ms = 0.0;
    uint32_t paths_planned = 0;
    uint32_t flow_builds_started = 0;
};

class NavSystem : public coopa::scene::ISceneSystem {
public:
    explicit NavSystem(nav::NavSettings settings = {})
        : settings_(std::move(settings)), baker_(settings_.build) {
        crowds_.resize(baker_.agent_count());
    }

    ~NavSystem() override {
        retire_all_flow_jobs_();
        if (path_batch_ && path_batch_->handle.is_valid()) {
            while (!path_batch_->handle.is_complete()) std::this_thread::yield();
            path_batch_->handle.close();
        }
    }

    const char* system_name() const override { return "Navigation"; }

    /**
     * @brief Not warm until the navmesh is built and no tile rebuild is outstanding.
     *
     * Only participates when the build will actually happen while a scene warms: the first full
     * build otherwise waits for a NavAgent to exist (see execute()), which for a scene that
     * spawns its agents from gameplay is well after the reveal -- so a scene that wants its
     * navmesh built behind the loading screen sets `build_without_agents`. Reporting
     * `participates` without that would hold the screen up until the warm-up timed out.
     */
    coopa::scene::WarmupState warm_up_state() const override {
        coopa::scene::WarmupState ws;
        ws.participates = settings_.enabled && (settings_.build_without_agents || baker_.built());
        if (!ws.participates) return ws;
        ws.what = "navigation";
        ws.warm = baker_.built() && !baker_.busy();
        ws.remaining = static_cast<int>(baker_.dirty_tile_count());
        ws.progress = ws.warm ? 1.0f : (baker_.built() ? 0.5f : 0.0f);
        return ws;
    }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        if (!settings_.enabled) return;
        auto* physics = dynamic_cast<PhysicsSystem*>(scene.find_system("Physics"));
        if (!physics) return;
        jobs_ = ctx.jobs;
        const float dt = ctx.delta_time;
        time_ += static_cast<double>(dt);
        // Idle until something uses navigation (see NavSettings::build_without_agents).
        if (!settings_.build_without_agents && !baker_.built() &&
            !scene.find_first_component<components::NavAgentComponent>()) {
            return;
        }

        StageTimer timer(stats_);
        // Colliders, volumes and links are rescanned on an interval, not every frame: a scan
        // walks every physics shape and the scene's components, and a change reaching the
        // navmesh 0.1 s later is invisible next to rebuild_delay anyway.
        scan_timer_ -= dt;
        if (scan_timer_ <= 0.0f || !baker_.built()) {
            scan_timer_ = settings_.source_scan_interval;
            gather_inputs_(scene, physics->world());
        }
        timer.lap(stats_.gather_ms);
        baker_.update(dt, ctx.jobs);
        timer.lap(stats_.build_ms);
        if (!baker_.built()) return;

        sync_agents_(scene);
        timer.lap(stats_.agents_ms);
        update_flow_fields_(scene);
        timer.lap(stats_.flow_ms);
        dispatch_paths_(scene, dt);
        timer.lap(stats_.paths_ms);
        for (std::size_t p = 0; p < crowds_.size(); ++p) {
            auto mesh = baker_.mesh(static_cast<uint32_t>(p));
            if (mesh && crowds_[p].active_count() > 0) crowds_[p].update(dt, *mesh, ctx.jobs, settings_.parallel_threshold);
        }
        timer.lap(stats_.crowd_ms);
        write_back_(dt);
        timer.lap(stats_.write_back_ms);
        close_finished_flow_jobs_();
    }

    // --- Queries for gameplay code ----------------------------------------------------------

    const nav::NavSettings& settings() const { return settings_; }
    nav::NavBaker& baker() { return baker_; }
    const nav::NavBaker& baker() const { return baker_; }

    /** @brief The current mesh of agent profile `agent_type` (empty = the first). */
    std::shared_ptr<const nav::NavMesh> mesh(const std::string& agent_type = {}) const {
        return baker_.mesh(profile_index_(agent_type));
    }

    /** @brief Synchronous A* on the current mesh. */
    nav::PathStatus find_path(const glm::vec3& start, const glm::vec3& goal, nav::NavPath& out,
                              const std::string& agent_type = {}) const {
        auto m = mesh(agent_type);
        if (!m) {
            out = nav::NavPath{};
            return nav::PathStatus::NoPath;
        }
        return nav::find_path(*m, start, goal, m->default_filter(), out);
    }

    /** @brief The current flow field toward `target` (an object name), if any agent follows it. */
    std::shared_ptr<const nav::FlowField> flow_field(const std::string& target, const std::string& agent_type = {}) const {
        auto it = flows_.find(flow_key_(target, profile_index_(agent_type)));
        return it != flows_.end() ? it->second.field : nullptr;
    }

    /** @brief Forces a full rebuild on the next frame. */
    void rebuild() { baker_.set_settings(settings_.build); }

    /** @brief Pace incremental rebuilds differently without a full rebuild -- NavBaker::set_rebuild_pacing(). */
    void set_rebuild_pacing(float rebuild_delay, int max_concurrent_rebuilds) {
        baker_.set_rebuild_pacing(rebuild_delay, max_concurrent_rebuilds);
    }
    /** @brief Seconds between collider/volume/link rescans (NavSettings::source_scan_interval). */
    void set_source_scan_interval(float seconds) {
        settings_.source_scan_interval = std::max(0.0f, seconds);
    }

    /** @brief Blocks until background tile and flow-field builds have landed (tests, loading). */
    void flush() {
        baker_.flush(jobs_);
        if (path_batch_ && path_batch_->handle.is_valid()) {
            if (jobs_) jobs_->wait_for(path_batch_->handle);
            else while (!path_batch_->handle.is_complete()) std::this_thread::yield();
        }
        for (auto& [key, f] : flows_) {
            if (f.job && f.job->handle.is_valid()) {
                if (jobs_) jobs_->wait_for(f.job->handle);
                else while (!f.job->handle.is_complete()) std::this_thread::yield();
            }
        }
    }

    std::size_t agent_count() const { return agents_.size(); }

    /** @brief Timings of the last execute() -- what navigation costs the frame. */
    const NavFrameStats& stats() const { return stats_; }

    /** @brief Emits navigation debug geometry (see nav::NavDebugFlags). */
    void debug_draw(debug::DebugDraw& out, nav::NavDebugFlags flags) const {
        if (flags == nav::NavDebugFlags::None) return;
        // Mesh and flow geometry only change when the mesh or a field is rebuilt: regenerate
        // them then, not every frame (a 40 m scene's outline + arrows is ~10k lines).
        const uint32_t mesh_bits = static_cast<uint32_t>(flags) &
            static_cast<uint32_t>(nav::NavDebugFlags::Mesh | nav::NavDebugFlags::Grid | nav::NavDebugFlags::Tiles | nav::NavDebugFlags::Links);
        if (auto m = baker_.mesh(0); m && mesh_bits) {
            if (m.get() != debug_cache_.mesh || mesh_bits != debug_cache_.mesh_bits) {
                debug_cache_.mesh_lines.lines.clear();
                nav::debug_draw_mesh(*m, debug_cache_.mesh_lines, flags);
                debug_cache_.mesh = m.get();
                debug_cache_.mesh_bits = mesh_bits;
                debug_cache_.mesh_hold = m;
            }
            out.lines.insert(out.lines.end(), debug_cache_.mesh_lines.lines.begin(), debug_cache_.mesh_lines.lines.end());
        }
        if (has_flag(flags, nav::NavDebugFlags::Flow) || has_flag(flags, nav::NavDebugFlags::FlowTiles)) {
            // One colour pair per field (by key order), so overlapping fields read apart.
            static constexpr uint32_t k_palette[4][2] = {{0x40FF90FFu, 0xC0A040FFu}, {0x60B0FFFFu, 0x8060E0FFu},
                                                         {0xFF70D0FFu, 0xD05050FFu}, {0xFFFF60FFu, 0x909090FFu}};
            std::vector<std::string> keys;
            for (const auto& [key, f] : flows_) keys.push_back(key);
            std::sort(keys.begin(), keys.end());
            for (const auto& [key, f] : flows_) {
                if (!f.field) continue;
                const std::size_t ci = static_cast<std::size_t>(std::find(keys.begin(), keys.end(), key) - keys.begin()) % 4;
                auto& entry = debug_cache_.flows[key];
                if (entry.field != f.field || entry.flags != static_cast<uint32_t>(flags)) {
                    entry.field = f.field;
                    entry.flags = static_cast<uint32_t>(flags);
                    entry.lines.lines.clear();
                    if (has_flag(flags, nav::NavDebugFlags::Flow)) nav::debug_draw_flow(*f.field, entry.lines, settings_.debug_flow_stride);
                    if (has_flag(flags, nav::NavDebugFlags::FlowTiles)) {
                        nav::debug_draw_flow_tiles(*f.field, entry.lines, 0.1f, k_palette[ci][0], k_palette[ci][1]);
                    }
                }
                out.lines.insert(out.lines.end(), entry.lines.lines.begin(), entry.lines.lines.end());
            }
        }
        if (has_flag(flags, nav::NavDebugFlags::Paths) || has_flag(flags, nav::NavDebugFlags::Agents)) {
            for (const auto& [comp, b] : agents_) {
                const nav::CrowdAgent& a = crowds_[b.profile].agent(b.id);
                if (has_flag(flags, nav::NavDebugFlags::Paths) && a.mode == nav::CrowdMode::Path) {
                    nav::debug_draw_path(a.path, out);
                }
                if (has_flag(flags, nav::NavDebugFlags::Agents)) {
                    glm::vec3 c = a.position + glm::vec3(0.0f, 0.0f, 0.05f);
                    float r = a.params.radius;
                    for (int i = 0; i < 12; ++i) {
                        float t0 = glm::two_pi<float>() * static_cast<float>(i) / 12.0f;
                        float t1 = glm::two_pi<float>() * static_cast<float>(i + 1) / 12.0f;
                        out.add_line(c + glm::vec3(std::cos(t0) * r, std::sin(t0) * r, 0.0f),
                                     c + glm::vec3(std::cos(t1) * r, std::sin(t1) * r, 0.0f), nav::colors::k_nav_agent);
                    }
                    out.add_line(c, c + glm::vec3(glm::vec2(a.velocity) * 0.5f, 0.0f), nav::colors::k_nav_agent);
                }
            }
        }
    }

private:
    /** @brief Writes elapsed time into successive stats fields; total on destruction. */
    struct StageTimer {
        using clock = std::chrono::steady_clock;
        NavFrameStats& stats;
        clock::time_point start = clock::now();
        clock::time_point last = start;
        explicit StageTimer(NavFrameStats& s) : stats(s) { s = NavFrameStats{}; }
        void lap(double& field) {
            clock::time_point now = clock::now();
            field = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
        }
        ~StageTimer() { stats.total_ms = std::chrono::duration<double, std::milli>(clock::now() - start).count(); }
    };

    struct ModifierRef {
        uint32_t generation = 0;
        const components::NavModifierComponent* modifier = nullptr;
    };

    enum class Mode : uint8_t { Idle, Point, Chase, Flow };

    struct AgentBinding {
        uint32_t profile = 0;
        nav::Crowd::AgentId id = nav::Crowd::k_invalid_agent;
        uint32_t last_command = ~0u;
        Mode mode = Mode::Idle;
        glm::vec3 last_written{0.0f};
        glm::vec3 planned_goal{0.0f};
        float repath_timer = 0.0f;
        bool needs_path = false;
        bool queued = false;
        bool in_flight = false;     /**< A path request for this agent is running. */
        uint64_t serial = 0;        /**< Unique per binding, so a late result can't land on a reused address. */
        uint64_t path_mesh_version = 0;
        bool was_arrived = false;
        float yaw = 0.0f;
        std::string flow_key;
    };

    struct FlowJob {
        coopa::job::JobHandle handle;
        std::shared_ptr<nav::FlowFieldBuilder> builder;
    };

    struct FlowEntry {
        std::string target;
        uint32_t profile = 0;
        std::shared_ptr<const nav::FlowField> field;
        glm::vec3 built_goal{0.0f};
        std::shared_ptr<FlowJob> job;
        glm::vec3 job_goal{0.0f};
        bool used = false;
        std::vector<glm::vec3> followers;
        bool follower_left_field = false;
        double last_kick = -1e9;
    };

    uint32_t profile_index_(const std::string& name) const {
        return name.empty() ? 0u : settings_.build.agent_index(name);
    }

    static std::string flow_key_(const std::string& target, uint32_t profile) {
        return target + "#" + std::to_string(profile);
    }

    static glm::vec3 world_position_(coopa::scene::SceneObject* obj) {
        auto* tc = obj ? obj->get_transform() : nullptr;
        return tc ? glm::vec3(tc->get_world_matrix()[3]) : glm::vec3(0.0f);
    }

    // --- 1. Inputs ----------------------------------------------------------------------------

    void gather_inputs_(coopa::scene::Scene& scene, const PhysicsWorld& world) {
        // NavModifiers, shallowest first so a nearer (deeper) modifier overwrites its ancestors'.
        mod_by_body_.clear();
        auto mods = scene.get_components<components::NavModifierComponent>();
        auto depth = [](const coopa::scene::SceneObject* o) {
            int d = 0;
            for (; o; o = o->parent()) ++d;
            return d;
        };
        std::stable_sort(mods.begin(), mods.end(), [&](auto* a, auto* b) { return depth(a->owner) < depth(b->owner); });
        for (const components::NavModifierComponent* m : mods) {
            if (!m->owner) continue;
            auto assign = [&](const coopa::scene::SceneObject& o) {
                for (const auto& c : o.components()) {
                    auto* col = dynamic_cast<const components::Collider*>(c.get());
                    if (!col || !col->body_id().is_valid()) continue;
                    mod_by_body_[col->body_id().index] = ModifierRef{col->body_id().generation, m};
                }
            };
            if (m->apply_to_children) m->owner->for_each_recursive(assign);
            else assign(*m->owner);
        }

        nav::SourceFilter filter;
        filter.layer_mask = settings_.build.layer_mask;
        filter.classify = [&](dynamics::BodyId id, uint32_t, const dynamics::Body& body, bool include, nav::SourceShape& s) {
            auto it = mod_by_body_.find(id.index);
            const components::NavModifierComponent* m =
                (it != mod_by_body_.end() && it->second.generation == id.generation) ? it->second.modifier : nullptr;
            if (!m) return include;
            if (m->ignore) return false;
            if (body.type == dynamics::BodyType::Dynamic && (!m->carve || body.awake)) return false;
            s.area = m->walkable ? settings_.build.areas.find(m->area) : nav::k_area_null;
            return true;
        };
        nav::gather_sources(world, filter, sources_scratch_);
        baker_.set_sources(sources_scratch_);

        std::vector<nav::SourceVolume> volumes;
        for (auto* v : scene.get_components<components::NavVolumeComponent>()) {
            auto* tc = v->owner ? v->owner->get_transform() : nullptr;
            if (!tc) continue;
            util::Trs trs = util::world_trs(tc->transform());
            nav::SourceVolume sv;
            sv.box.center = trs.position + trs.rotation * (v->center * trs.scale);
            sv.box.half_extents = 0.5f * glm::abs(v->size * trs.scale);
            sv.box.orientation = trs.rotation;
            sv.area = v->area == "NotWalkable" ? nav::k_area_null : settings_.build.areas.find(v->area);
            sv.fingerprint = nav::fingerprint(sv);
            volumes.push_back(sv);
        }
        baker_.set_volumes(std::move(volumes));

        std::vector<nav::OffMeshLink> links;
        for (auto* l : scene.get_components<components::NavLinkComponent>()) {
            auto* tc = l->owner ? l->owner->get_transform() : nullptr;
            if (!tc) continue;
            glm::mat4 m = tc->get_world_matrix();
            nav::OffMeshLink ol;
            ol.start = glm::vec3(m * glm::vec4(l->start, 1.0f));
            ol.end = glm::vec3(m * glm::vec4(l->end, 1.0f));
            ol.bidirectional = l->bidirectional;
            ol.cost = l->cost;
            ol.area = settings_.build.areas.find(l->area);
            ol.snap_radius = l->snap_radius;
            ol.user_id = static_cast<uint32_t>(links.size());
            links.push_back(ol);
        }
        baker_.set_links(std::move(links));
    }

    // --- 2. Agents ----------------------------------------------------------------------------

    nav::CrowdAgentParams params_for_(const components::NavAgentComponent& c, uint32_t profile) const {
        nav::CrowdAgentParams p;
        const nav::AgentProfile& prof = settings_.build.agents[profile];
        p.radius = c.radius >= 0.0f ? c.radius : prof.radius;
        p.height = prof.height;
        p.max_speed = c.speed;
        p.max_acceleration = c.acceleration;
        p.stopping_distance = c.stopping_distance;
        p.slowdown_distance = c.slowdown_distance;
        p.separation_weight = c.separation_weight;
        p.filter.area_cost = settings_.build.areas.costs;
        p.filter.heuristic_weight = c.heuristic_weight;
        for (const std::string& a : c.avoid_areas) {
            uint8_t id = settings_.build.areas.find(a, nav::k_area_null);
            if (id != nav::k_area_null) p.filter.area_mask &= ~(1ull << id);
        }
        return p;
    }

    void sync_agents_(coopa::scene::Scene& scene) {
        auto comps = scene.get_components<components::NavAgentComponent>();
        std::unordered_map<components::NavAgentComponent*, bool> seen;
        seen.reserve(comps.size());
        for (auto* c : comps) {
            if (!c->owner || !c->owner->get_transform()) continue;
            seen[c] = true;
            auto it = agents_.find(c);
            uint32_t profile = profile_index_(c->agent_type);
            if (it != agents_.end() && it->second.profile != profile) {
                crowds_[it->second.profile].remove_agent(it->second.id);
                agents_.erase(it);
                it = agents_.end();
            }
            glm::vec3 pos = world_position_(c->owner);
            if (it == agents_.end()) {
                AgentBinding b;
                b.serial = ++next_serial_;
                b.profile = profile;
                b.id = crowds_[profile].add_agent(pos - glm::vec3(0.0f, 0.0f, c->base_offset), params_for_(*c, profile));
                b.last_written = pos;
                glm::vec3 fwd = glm::mat3(c->owner->get_transform()->get_world_matrix()) * forward_axis_(c->forward);
                b.yaw = std::atan2(fwd.y, fwd.x);
                it = agents_.emplace(c, b).first;
            }
            AgentBinding& b = it->second;
            nav::Crowd& crowd = crowds_[profile];
            crowd.agent(b.id).params = params_for_(*c, profile);

            glm::vec3 warp;
            if (c->take_warp(warp)) {
                crowd.warp(b.id, warp - glm::vec3(0.0f, 0.0f, c->base_offset));
                b.needs_path = b.mode == Mode::Point || b.mode == Mode::Chase;
            } else if (glm::distance(pos, b.last_written) > 1e-3f) {
                // Something else moved the object (a script, the editor): follow it.
                crowd.warp(b.id, pos - glm::vec3(0.0f, 0.0f, c->base_offset));
                b.needs_path = b.mode == Mode::Point || b.mode == Mode::Chase;
            }

            if (c->command_revision() != b.last_command) {
                b.last_command = c->command_revision();
                Mode mode = !c->flow_target.empty() ? Mode::Flow
                          : !c->destination_object.empty() ? Mode::Chase
                          : c->has_destination() ? Mode::Point : Mode::Idle;
                b.mode = mode;
                b.was_arrived = false;
                b.needs_path = mode == Mode::Point || mode == Mode::Chase;
                b.repath_timer = 0.0f;
                if (mode == Mode::Idle || mode == Mode::Flow) crowd.stop(b.id);
                b.flow_key = mode == Mode::Flow ? flow_key_(c->flow_target, profile) : std::string();
                c->set_path_pending_(b.needs_path);
            }
        }
        for (auto it = agents_.begin(); it != agents_.end();) {
            if (!seen.count(it->first)) {
                crowds_[it->second.profile].remove_agent(it->second.id);
                it = agents_.erase(it);
            } else {
                ++it;
            }
        }
    }

    static glm::vec3 forward_axis_(const std::string& f) {
        if (f == "X" || f == "+X") return {1.0f, 0.0f, 0.0f};
        if (f == "-X") return {-1.0f, 0.0f, 0.0f};
        if (f == "-Y") return {0.0f, -1.0f, 0.0f};
        return {0.0f, 1.0f, 0.0f};
    }

    // --- 3. Flow fields -----------------------------------------------------------------------

    void update_flow_fields_(coopa::scene::Scene& scene) {
        for (auto& [key, f] : flows_) {
            f.used = false;
            f.followers.clear();
            f.follower_left_field = false;
        }
        for (auto& [comp, b] : agents_) {
            if (b.mode != Mode::Flow) continue;
            FlowEntry& f = flows_[b.flow_key];
            if (f.target.empty()) {
                f.target = comp->flow_target;
                f.profile = b.profile;
            }
            f.used = true;
            const nav::CrowdAgent& a = crowds_[b.profile].agent(b.id);
            f.followers.push_back(a.position);
            if (a.flow && a.flow == f.field && (!a.flow_valid || a.flow_coarse) && !a.on_link) f.follower_left_field = true;
        }
        for (auto it = flows_.begin(); it != flows_.end();) {
            if (it->second.used) {
                ++it;
                continue;
            }
            if (it->second.job) retired_jobs_.push_back(it->second.job);
            it = flows_.erase(it);
        }

        for (auto& [key, f] : flows_) {
            auto mesh = baker_.mesh(f.profile);
            if (!mesh) continue;
            // Land a finished background build.
            if (f.job && f.job->handle.is_complete()) {
                f.job->handle.close();
                f.field = f.job->builder->finish();
                f.built_goal = f.job_goal;
                f.job.reset();
            }
            coopa::scene::SceneObject* target = scene.find_object(f.target);
            if (!target) continue;
            glm::vec3 goal = world_position_(target);
            // Followers outside the exact area steer by the coarse layer meanwhile, so that
            // rebuild is throttled; a moved target or a new mesh rebuilds right away.
            bool followers_out = f.follower_left_field && time_ - f.last_kick >= settings_.flow_rebuild_interval;
            bool stale = !f.field || f.field->mesh().version != mesh->version ||
                         glm::distance(goal, f.built_goal) > settings_.flow_rebuild_distance || followers_out;
            if (!stale || f.job) continue;
            f.last_kick = time_;

            nav::FlowFieldSettings fs;
            fs.filter = mesh->default_filter();
            fs.filter.wall_penalty = settings_.flow_wall_penalty;
            fs.max_distance = settings_.flow_max_distance;
            // March only as far as the followers need (plus a margin) -- see
            // FlowFieldSettings::required_points.
            fs.mode = settings_.flow_mode;
            fs.exact_tile_budget = settings_.flow_exact_tile_budget;
            fs.near_radius = settings_.flow_near_radius;
            fs.lookahead_tiles = settings_.flow_lookahead_tiles;
            // Followers always steer the hierarchical planner; bound_to_followers decides whether
            // an exact field also stops once they are covered.
            fs.required_points = f.followers;
            fs.bound_exact_to_required = settings_.flow_bound_to_followers;
            if (!f.field || !jobs_) {
                // First field (agents would otherwise idle until it lands), or no job engine.
                glm::vec3 goals[1] = {goal};
                f.field = nav::FlowField::build(mesh, goals, fs, jobs_);
                f.built_goal = goal;
                ++stats_.flow_builds_started;
                continue;
            }
            ++stats_.flow_builds_started;
            auto job = std::make_shared<FlowJob>();
            job->handle = jobs_->create_handle();
            glm::vec3 goals[1] = {goal};
            job->builder = std::make_shared<nav::FlowFieldBuilder>(mesh, goals, fs);
            f.job = job;
            f.job_goal = goal;
            // A chain of short Low-priority slices, never one long job -- see
            // submit_flow_field_build()'s doc for why that matters to the frame.
            nav::submit_flow_field_build(*jobs_, job->handle, job->builder);
        }

        for (auto& [comp, b] : agents_) {
            if (b.mode != Mode::Flow) continue;
            auto it = flows_.find(b.flow_key);
            nav::Crowd& crowd = crowds_[b.profile];
            if (it == flows_.end() || !it->second.field) continue;
            if (crowd.agent(b.id).flow != it->second.field) crowd.set_flow(b.id, it->second.field);
        }
    }

    void close_finished_flow_jobs_() {
        for (auto it = retired_jobs_.begin(); it != retired_jobs_.end();) {
            if ((*it)->handle.is_complete()) {
                (*it)->handle.close();
                it = retired_jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void retire_all_flow_jobs_() {
        for (auto& [key, f] : flows_) {
            if (f.job) retired_jobs_.push_back(f.job);
        }
        flows_.clear();
        for (auto& j : retired_jobs_) {
            while (!j->handle.is_complete()) std::this_thread::yield();
            j->handle.close();
        }
        retired_jobs_.clear();
    }

    // --- 4. Paths -----------------------------------------------------------------------------

    struct PendingPath {
        components::NavAgentComponent* comp = nullptr;
        uint64_t serial = 0;           /**< AgentBinding::serial at request time. */
        uint32_t command = 0;          /**< The agent's command revision at request time. */
        std::shared_ptr<const nav::NavMesh> mesh;
        glm::vec3 start{0.0f};
        glm::vec3 goal{0.0f};
        nav::QueryFilter filter;
        nav::NavPath result;
    };

    /** @brief One frame's A* requests, running as background jobs (see dispatch_paths_()). */
    struct PathBatch {
        std::vector<PendingPath> items;
        coopa::job::JobHandle handle;
    };

    /**
     * @brief Queues agents that need a path, applies the previous batch once it has finished,
     *        and starts the next batch -- without ever waiting on A* from the frame: queries run
     *        as Low-priority jobs against the (immutable) mesh snapshot they were issued on, and
     *        land a frame or two later. A result whose agent was removed or re-commanded in the
     *        meantime is dropped.
     */
    void dispatch_paths_(coopa::scene::Scene& scene, float dt) {
        if (path_batch_ && (!path_batch_->handle.is_valid() || path_batch_->handle.is_complete())) {
            apply_path_batch_(*path_batch_);
            path_batch_->handle.close();
            path_batch_.reset();
        }

        for (auto& [comp, b] : agents_) {
            if (b.mode != Mode::Point && b.mode != Mode::Chase) continue;
            auto mesh = baker_.mesh(b.profile);
            if (!mesh) continue;
            const nav::CrowdAgent& a = crowds_[b.profile].agent(b.id);
            if (b.mode == Mode::Chase) {
                b.repath_timer -= dt;
                coopa::scene::SceneObject* target = scene.find_object(comp->destination_object);
                if (target && b.repath_timer <= 0.0f && !b.in_flight &&
                    glm::distance(world_position_(target), b.planned_goal) > settings_.repath_distance) {
                    b.needs_path = true;
                }
            }
            // A changed mesh may have opened a shortcut or closed the route: re-plan.
            if (b.path_mesh_version != mesh->version && !b.in_flight && !a.arrived && a.mode == nav::CrowdMode::Path) {
                b.needs_path = true;
            }
            if (b.needs_path && !b.queued) {
                b.queued = true;
                queue_.push_back(comp);
            }
        }
        if (path_batch_ || queue_.empty()) return; // one batch in flight at a time

        std::size_t budget = static_cast<std::size_t>(std::max(1, settings_.max_path_requests_per_frame));
        auto batch = std::make_shared<PathBatch>();
        std::size_t consumed = 0;
        for (; consumed < queue_.size() && batch->items.size() < budget; ++consumed) {
            components::NavAgentComponent* comp = queue_[consumed];
            auto it = agents_.find(comp);
            if (it == agents_.end()) continue; // agent removed while queued
            AgentBinding& b = it->second;
            b.queued = false;
            if (!b.needs_path || (b.mode != Mode::Point && b.mode != Mode::Chase)) continue;
            PendingPath p;
            p.comp = comp;
            p.serial = b.serial;
            p.command = b.last_command;
            p.mesh = baker_.mesh(b.profile);
            if (!p.mesh) continue;
            const nav::CrowdAgent& a = crowds_[b.profile].agent(b.id);
            p.start = a.position;
            if (b.mode == Mode::Chase) {
                coopa::scene::SceneObject* target = scene.find_object(comp->destination_object);
                if (!target) continue;
                p.goal = world_position_(target);
            } else {
                p.goal = comp->destination();
            }
            p.filter = a.params.filter;
            b.needs_path = false;
            b.in_flight = true;
            batch->items.push_back(std::move(p));
        }
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(consumed));
        if (batch->items.empty()) return;
        stats_.paths_planned = static_cast<uint32_t>(batch->items.size());

        if (!jobs_) {
            for (PendingPath& p : batch->items) nav::find_path(*p.mesh, p.start, p.goal, p.filter, p.result);
            apply_path_batch_(*batch);
            return;
        }
        batch->handle = jobs_->create_handle();
        for (std::size_t i = 0; i < batch->items.size(); ++i) {
            jobs_->submit([batch, i]() {
                PendingPath& p = batch->items[i];
                nav::find_path(*p.mesh, p.start, p.goal, p.filter, p.result);
            }, 0, batch->handle, nullptr, 0u, coopa::job::Priority::Low);
        }
        path_batch_ = std::move(batch);
    }

    void apply_path_batch_(PathBatch& batch) {
        for (PendingPath& p : batch.items) {
            auto it = agents_.find(p.comp);
            if (it == agents_.end() || it->second.serial != p.serial) continue; // agent gone
            AgentBinding& b = it->second;
            b.in_flight = false;
            if (b.last_command != p.command) continue; // re-commanded meanwhile: a newer request is queued
            b.planned_goal = p.goal;
            b.path_mesh_version = p.mesh->version;
            b.repath_timer = settings_.repath_interval;
            b.was_arrived = false;
            crowds_[b.profile].set_path(b.id, p.result);
            p.comp->set_path_(std::move(p.result));
        }
    }

    // --- 5. Write back ------------------------------------------------------------------------

    void write_back_(float dt) {
        for (auto& [comp, b] : agents_) {
            const nav::CrowdAgent& a = crowds_[b.profile].agent(b.id);
            auto* tc = comp->owner->get_transform();
            glm::vec3 pos = a.position + glm::vec3(0.0f, 0.0f, comp->base_offset);
            util::Trs current = util::world_trs(tc->transform());
            glm::quat rot = current.rotation;
            glm::vec2 v(a.velocity);
            if (comp->update_rotation && glm::length(v) > 0.05f) {
                glm::vec3 f = forward_axis_(comp->forward);
                float target = std::atan2(v.y, v.x) - std::atan2(f.y, f.x);
                float diff = std::remainder(target - b.yaw, glm::two_pi<float>());
                float max_step = comp->angular_speed > 0.0f ? glm::radians(comp->angular_speed) * dt : std::fabs(diff);
                b.yaw += std::clamp(diff, -max_step, max_step);
                rot = glm::angleAxis(b.yaw, glm::vec3(0.0f, 0.0f, 1.0f));
            }
            util::set_world_trs(tc->transform(), pos, rot);
            b.last_written = pos;

            bool arrived = (b.mode != Mode::Idle) && a.arrived;
            comp->set_state_(arrived, a.velocity, a.remaining_distance(), a.ref != nav::k_invalid_span);
            if (arrived && !b.was_arrived) comp->on_arrived.emit(*comp);
            b.was_arrived = arrived;
        }
    }

    nav::NavSettings settings_;
    nav::NavBaker baker_;
    std::vector<nav::Crowd> crowds_;
    coopa::job::JobEngine* jobs_ = nullptr;

    std::unordered_map<uint32_t, ModifierRef> mod_by_body_;
    std::vector<nav::SourceShape> sources_scratch_;
    std::unordered_map<components::NavAgentComponent*, AgentBinding> agents_;
    std::vector<components::NavAgentComponent*> queue_;
    std::unordered_map<std::string, FlowEntry> flows_;
    std::vector<std::shared_ptr<FlowJob>> retired_jobs_;
    NavFrameStats stats_;
    float scan_timer_ = 0.0f;
    double time_ = 0.0;
    std::shared_ptr<PathBatch> path_batch_;
    uint64_t next_serial_ = 0;

    struct DebugCache {
        const nav::NavMesh* mesh = nullptr;
        std::shared_ptr<const nav::NavMesh> mesh_hold; /**< Keeps `mesh` from being reused by a new allocation. */
        uint32_t mesh_bits = 0;
        debug::DebugDraw mesh_lines;
        struct Flow {
            std::shared_ptr<const nav::FlowField> field;
            uint32_t flags = 0;
            debug::DebugDraw lines;
        };
        std::unordered_map<std::string, Flow> flows;
    };
    mutable DebugCache debug_cache_;
};

/**
 * @brief Registers a NavSystem on `scene` and returns it.
 * @param order Defaults to 150: after Physics (100) -- the system reads the physics world --
 *        and before the Behaviour walk (200).
 */
inline NavSystem* install_nav_system(coopa::scene::Scene& scene, const nav::NavSettings& settings = {}, int order = 150) {
    auto sys = std::make_unique<NavSystem>(settings);
    NavSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

/** @brief The scene's NavSystem, or nullptr. */
inline NavSystem* find_nav_system(coopa::scene::Scene& scene) {
    return dynamic_cast<NavSystem*>(scene.find_system("Navigation"));
}

} // namespace system
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_SYSTEM_NAV_SYSTEM_H
