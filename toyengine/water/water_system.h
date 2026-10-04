/**
 * @file water_system.h
 * @brief Bakes every WaterBody's surface (GPU mesh + CPU query) and drives every Buoyancy body
 *        from inside the physics substep.
 *
 * Ordering. Installed at k_water_system_order (90): just ahead of Physics (100). execute()
 * refreshes the list of buoyant bodies right before PhysicsSystem steps, so the pointers the
 * substep callback walks are never older than the current frame (a component destroyed during
 * the Behaviour walk or the command flush is dropped from the list before the next step).
 *
 * Bake. A body is baked in up to two stages:
 *  1. As soon as its geometry exists -- so frame 0 already draws water, and so does the editor
 *     (this system runs in edit mode, baking only) -- with whatever physics knows. Before
 *     PhysicsSystem has gathered colliders (it runs after this system, and not at all in edit
 *     mode) that bake has no depth or obstacle information.
 *  2. Once the scene simulates and PhysicsSystem has bodies, again WITH raycasts against static
 *     colliders: per-vertex
 *     water depth (calms waves toward the shore, see water_waves.h) and, for flowing water,
 *     obstacle deflection and wakes (water_flow_bake.h). The mesh is re-uploaded once.
 * A body whose Transform moves is re-baked (water bodies are expected to be static; this keeps
 * an editor drag correct rather than fast). Stage 2 is the expensive one (raycasts per vertex):
 * it waits until the body is within WaterSettings::sim_radius of the focus and runs for at most
 * WaterSettings::bakes_per_frame bodies a frame, nearest first, so a big world neither hitches
 * on its first simulated frame nor bakes lakes nobody is near.
 *
 * Quality and range (water_settings.h). set_settings() picks a tier. Distances are measured from
 * the focus: set_focus(), else the main camera of this scene, else none (no focus = everything
 * is in range, as in a headless test with no camera). Floaters outside sim_radius are frozen
 * (see substep_()); ripples come only from bodies within ripple_range.
 *
 * Tiles. A body larger than one render tile (WaterBody::tile_size; by default 32 m, at most
 * k_max_tiles_per_side a side) is published as runtime CHILD objects, one MeshRenderer per tile
 * with its own LOD chain (water_tiles.h), so the renderer culls and LODs each tile -- the
 * pattern TerrainSystem uses for its chunks. A single-tile body keeps its mesh on its own
 * MeshRenderer. Tile renderers copy the owner MeshRenderer's material on every bake, and every
 * frame in edit mode (call sync_material() after changing it from gameplay). Tiles are not part
 * of the authored scene.
 *
 * GPU vertex packing (consumed by assets/shaders/water_surface.glsl):
 *   position  object space, undisplaced
 *   normal    object space surface normal
 *   uv        (depth metres, turbulence 0..1)
 *   tangent   (object-space flow velocity m/s, 2) -- w == 2 marks a baked water vertex
 * plus PBRMaterial::shader = "water", shader_params = WaveParams::pack(),
 * shader_params_ext = (foam rgb, foam amount), (shore foam depth, edge fade depth,
 * ripple strength, ripple scale), and refraction_thickness = clarity (the hook overwrites the
 * refraction thickness with the measured depth, so the slot is free to carry it).
 *
 * Ripples. Every frame, each Rigidbody crossing a surface may emit an expanding ring (splash on
 * entry, a wake trail while moving, a ring now and then while bobbing) -- see emit_ripples_().
 * The live rings (ripples()) are handed to the renderer by Engine and drawn by the water shader;
 * they are visual only (buoyancy does not feel them).
 *
 * Underwater. underwater_at() answers whether a point -- the camera -- is below a surface; Engine
 * turns that into PixelRenderPipeline's UnderwaterPass parameters.
 *
 * Buoyancy. One connection to PhysicsWorld::on_substep for the whole scene. on_substep fires
 * after integrate_forces() and before the solve (world.h's step_fixed()), so buoyancy is applied
 * as IMPULSES (Body::apply_impulse_at_position) to reach this substep's solve; a force would land
 * one substep late. Impulses are applied without waking the body so a crate floating on calm
 * water can still fall asleep; a sleeping body is only woken when the water under it is moving
 * (waves or current) -- otherwise it stays asleep at zero cost.
 */

#ifndef TOYENGINE_WATER_WATER_SYSTEM_H
#define TOYENGINE_WATER_WATER_SYSTEM_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/event/signal.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>

#include <physxcoopa/components/collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_flow_bake.h>
#include <toyengine/water/water_settings.h>
#include <toyengine/water/water_surface_query.h>
#include <toyengine/water/water_tiles.h>
#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

/** @brief Default registration order: just ahead of UpdatePhase::Physics (100). See file doc. */
inline constexpr int k_water_system_order = 90;

/** @brief Render tile edge (m) when WaterBody::tile_size is 0. See the file doc. */
inline constexpr float k_default_tile_size = 32.0f;

/**
 * @brief Auto tile size never cuts a body into more than this many tiles a side. Every tile is a
 *        renderer the CPU gathers, culls, LOD-selects and sorts each frame, plus a transform to
 *        resolve: measured on the water_stress ocean (1 km), ~1000 tiles of 32 m cost ~3 ms of
 *        CPU a frame over one mesh for no GPU gain, while 64 tiles cost a fraction of that.
 */
inline constexpr int k_max_tiles_per_side = 8;

/**
 * @class WaterSystem
 * @brief See the file doc.
 */
class WaterSystem : public coopa::scene::ISceneSystem {
public:
    /** @brief GPU arguments may be null (headless tests): bodies are then baked CPU-side only. */
    WaterSystem(coopa::gfx::core::Device* device = nullptr, coopa::gfx::memory::Allocator* allocator = nullptr,
                coopa::asset::AssetManager* assets = nullptr)
        : device_(device), allocator_(allocator), assets_(assets) {}

    ~WaterSystem() override { substep_connection_.disconnect(); }

    const char* system_name() const override { return "Water"; }

    /// Runs in the editor too, but only to BAKE: water bodies need their mesh published to be
    /// visible at all, simulating or not. Buoyancy and ripples stay off until the scene
    /// simulates (see execute()).
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        connect_physics_(scene);
        jobs_ = ctx.jobs;
        const bool simulating = scene.is_simulating();
        // Advanced per substep, in lockstep with physics, while simulating; otherwise (edit mode,
        // or no physics) by the frame clock so CPU queries still see moving waves.
        if (!physics_ || !simulating) time_ += ctx.delta_time;
        simulating_ = simulating;
        resolve_focus_(scene);

        // Depth and obstacle raycasts need PhysicsSystem's colliders, which it only gathers once
        // it has executed -- after this system on a scene's first simulated frame, and never in
        // edit mode. Until then bodies bake without them (stage 1) and are re-baked once.
        const bool physics_ready = physics_ != nullptr && simulating && physics_->bound_count() > 0;

        bodies_ = scene.get_components<WaterBody>();
        // Stage 1 for anything new, moved, or baked at another grid density: needed to be seen
        // at all, and cheap (no raycasts).
        for (WaterBody* body : bodies_) {
            if (!body->owner) continue;
            const glm::mat4& world = body->owner->get_transform()->transform().get_world_matrix();
            const bool moved = body->baked && world != body->baked_world;
            const bool density = body->baked && uses_grid_(*body) && body->baked_density != settings_.grid_density;
            if (body->baked && !moved && !density) continue;
            bake_(scene, *body, world, false);
        }
        // Stage 2, nearest first, within range, a few per frame.
        if (physics_ready) {
            std::vector<std::pair<float, WaterBody*>> pending;
            for (WaterBody* body : bodies_) {
                if (!body->owner || !body->baked || body->bake_stage >= 2) continue;
                const float d = distance_to_body_(*body);
                if (d <= settings_.sim_radius) pending.push_back({d, body});
            }
            std::sort(pending.begin(), pending.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            const std::size_t budget = static_cast<std::size_t>(std::max(settings_.bakes_per_frame, 1));
            for (std::size_t i = 0; i < pending.size() && i < budget; ++i) {
                WaterBody& body = *pending[i].second;
                bake_(scene, body, body.owner->get_transform()->transform().get_world_matrix(), true);
            }
        }

        if (!simulating) {
            // Edit mode: the bodies are visible; nothing floats and nothing ripples. The editor
            // edits the owner's material live, so keep tiles in step.
            for (WaterBody* body : bodies_) sync_material(*body);
            buoyant_.clear();
            ripples_.clear();
            emitters_.clear();
            active_bodies_.clear();
            return;
        }
        gather_active_bodies_();
        gather_buoyant_(scene);
        emit_ripples_(scene);
    }

    // ------------------------------------------------------------------------------------
    // Quality and range
    // ------------------------------------------------------------------------------------

    /** @brief Applies a quality tier (live). Procedural grids re-bake at the new density. */
    void set_settings(const WaterSettings& settings) {
        settings_ = settings;
        while (ripples_.size() > settings_.max_ripples) ripples_.erase(ripples_.begin());
    }
    const WaterSettings& settings() const { return settings_; }

    /** @brief Measures ranges from `p` instead of the main camera (until clear_focus()). */
    void set_focus(const glm::vec3& p) {
        focus_override_ = true;
        has_focus_ = true;
        focus_ = p;
    }
    void clear_focus() { focus_override_ = false; }
    /** @brief The point ranges were measured from on the last execute(); false = no focus. */
    bool focus(glm::vec3& out) const {
        if (has_focus_) out = focus_;
        return has_focus_;
    }

    /** @brief True when every render tile of `body` (or its own MeshRenderer) has a mesh. */
    static bool is_published(const WaterBody& body) {
        using coopa::gfx::engine::components::MeshRenderer;
        if (!body.baked || !body.owner) return false;
        if (body.tiles.empty()) {
            auto* mr = body.owner->get_component<MeshRenderer>();
            return mr && mr->is_ready();
        }
        for (const auto& t : body.tiles) {
            auto* mr = t.object ? t.object->get_component<MeshRenderer>() : nullptr;
            if (!mr || !mr->is_ready()) return false;
        }
        return true;
    }

    /** @brief Copies the owner MeshRenderer's material (and look flags) to every render tile. */
    static void sync_material(WaterBody& body) {
        using coopa::gfx::engine::components::MeshRenderer;
        if (body.tiles.empty() || !body.owner) return;
        auto* src = body.owner->get_component<MeshRenderer>();
        if (!src) return;
        for (auto& t : body.tiles) {
            auto* mr = t.object ? t.object->get_component<MeshRenderer>() : nullptr;
            if (!mr) continue;
            mr->material = src->material;
            mr->affects_reflection_probes = src->affects_reflection_probes;
            mr->lod_bias = src->lod_bias;
        }
    }

    /**
     * @brief Re-reads the scene's WaterBody list. sample()/underwater_at() walk the list taken at
     *        the last execute(); anything that may destroy or rebuild water objects between then
     *        and a query (the editor applies its edits after Scene::update()) must refresh first,
     *        or the query would read a destroyed component. Engine does, before every render.
     */
    void refresh_bodies(coopa::scene::Scene& scene) { bodies_ = scene.get_components<WaterBody>(); }

    /** @brief Water clock (seconds) -- the time every wave on the CPU is evaluated at. */
    float time() const { return time_; }

    /**
     * @brief The water clock as the renderer should draw waves at -- the shader's wave phase
     *        (Engine hands it over as WaterFrameState::time). While physics runs, rendered bodies
     *        are interpolated a fraction of a substep behind the last solve, so the drawn surface
     *        is pulled back by the same amount; otherwise it is time() itself.
     */
    float render_time() const {
        if (!physics_ || !simulating_) return time_;
        const auto& world = physics_->world();
        return time_ - world.config().fixed_dt * (1.0f - world.interpolation_alpha());
    }

    /**
     * @brief Samples the highest water surface above world XY `p` across every water body.
     * @param which Optional: the body that answered.
     * @return false if no body covers `p`.
     */
    bool sample(const glm::vec2& p, WaterSample& out, const WaterBody** which = nullptr) const {
        bool found = false;
        const WaveQueryOptions opt = wave_options_();
        for (const WaterBody* body : bodies_) {
            WaterSample s;
            if (!body->sample(p, time_, s, opt)) continue;
            if (!found || s.surface_height > out.surface_height) {
                out = s;
                found = true;
                if (which) *which = body;
            }
        }
        return found;
    }

    /** @brief Number of bodies buoyancy considered on the last substep (diagnostics/tests). */
    std::size_t active_buoyant_count() const { return active_count_; }

    // ------------------------------------------------------------------------------------
    // Ripples
    // ------------------------------------------------------------------------------------

    /** @brief One expanding ring on a water surface. See emit_ripples_() for the sources. */
    struct Ripple {
        glm::vec2 position{0.0f};  ///< World XY of the ring's centre.
        float     birth = 0.0f;    ///< time() at emission.
        float     strength = 0.0f; ///< Peak slope scale (shader) -- ~0.05 a nudge, ~1 a splash.
        float     radius = 0.0f;   ///< Starting radius: the emitter's waterline size.
    };
    /// Hard cap on live rings (the renderer's UBO size); the tier's WaterSettings::max_ripples
    /// is the working cap (oldest dropped first): the shader loops over all of them.
    static constexpr std::size_t k_max_ripples = 64;
    /// Seconds a ring lives; its amplitude has decayed to ~2% by then.
    static constexpr float k_ripple_lifetime = 3.0f;

    /** @brief Live rings, oldest first. */
    const std::vector<Ripple>& ripples() const { return ripples_; }

    /** @brief Adds a ring by hand (gameplay: a thrown stone, a footstep). */
    void emit_ripple(const glm::vec2& position, float strength, float radius) {
        ripples_.push_back({position, time_, strength, radius});
        const std::size_t cap = std::min(std::max<std::size_t>(settings_.max_ripples, 1), k_max_ripples);
        while (ripples_.size() > cap) ripples_.erase(ripples_.begin());
    }

    // ------------------------------------------------------------------------------------
    // Underwater
    // ------------------------------------------------------------------------------------

    /** @brief Where `p` sits relative to the water: see underwater_at(). */
    struct UnderwaterInfo {
        bool             underwater = false;
        float            surface_height = 0.0f; ///< Of the body above `p` (valid when `body`).
        float            depth = 0.0f;          ///< surface_height - p.z (negative above water).
        const WaterBody* body = nullptr;        ///< The body above/around `p`, or null.
    };

    /**
     * @brief Whether world point `p` (e.g. a camera) is under a water surface -- the highest
     *        surface above its XY, waves included -- and above that body's bed.
     */
    UnderwaterInfo underwater_at(const glm::vec3& p) const {
        UnderwaterInfo info;
        WaterSample s;
        const WaterBody* body = nullptr;
        if (!sample(glm::vec2(p), s, &body)) return info;
        info.body = body;
        info.surface_height = s.surface_height;
        info.depth = s.surface_height - p.z;
        info.underwater = info.depth > 0.0f && info.depth < s.depth + 1.0f;
        return info;
    }

private:
    // ------------------------------------------------------------------------------------
    // Physics hookup
    // ------------------------------------------------------------------------------------

    void connect_physics_(coopa::scene::Scene& scene) {
        auto* sys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
        if (sys == physics_) return;
        substep_connection_.disconnect();
        physics_ = sys;
        if (physics_) {
            substep_connection_ = physics_->world().on_substep.connect(
                [this](coopa::physx::PhysicsWorld& world, float h) { substep_(world, h); });
        }
    }

    // ------------------------------------------------------------------------------------
    // Focus and range
    // ------------------------------------------------------------------------------------

    /** @brief The focus: set_focus()'s point, else this scene's main camera, else none. */
    void resolve_focus_(coopa::scene::Scene& scene) {
        if (focus_override_) return;
        has_focus_ = false;
        auto* camera = coopa::gfx::engine::components::CameraComponent::main();
        // Only a camera of THIS scene: the main camera may belong to another (an editor's
        // edit scene under a pushed play scene, or a previous scene not yet torn down).
        if (!camera || !camera->owner || camera->scene != &scene) return;
        if (auto* tc = camera->owner->get_transform()) {
            // get_world_matrix(), not world_matrix(): this runs before TransformResolve (see
            // TerrainSystem::camera_position_()).
            focus_ = glm::vec3(tc->transform().get_world_matrix()[3]);
            has_focus_ = true;
        }
    }

    /** @brief Distance from the focus to a baked body's bounds (0 inside, or with no focus). */
    float distance_to_body_(const WaterBody& body) const {
        if (!has_focus_ || !body.query.valid()) return 0.0f;
        const glm::vec3 c = glm::clamp(focus_, body.query.bounds_min(), body.query.bounds_max());
        return glm::distance(c, focus_);
    }

    /** @brief How CPU wave samples are taken: the tier's inversion steps, faded like the GPU. */
    WaveQueryOptions wave_options_() const {
        WaveQueryOptions o;
        o.iterations = settings_.wave_iterations;
        o.has_focus = has_focus_;
        o.focus = focus_;
        return o;
    }

    /** @brief Bodies near enough to the focus that a simulated floater could touch them. */
    void gather_active_bodies_() {
        active_bodies_.clear();
        // A floater stays simulated out to sim_radius * hysteresis; its own size is small next
        // to the margin.
        const float reach = settings_.sim_radius * settings_.sim_hysteresis + 25.0f;
        for (WaterBody* body : bodies_) {
            if (body->baked && distance_to_body_(*body) <= reach) active_bodies_.push_back(body);
        }
    }

    // ------------------------------------------------------------------------------------
    // Bake
    // ------------------------------------------------------------------------------------

    /** @brief Whether `body` is a procedural grid (no mesh geometry of its own). */
    static bool uses_grid_(const WaterBody& body) {
        return body.geometry_indices.empty() && body.mesh_path.empty();
    }

    /** @brief A procedural body's grid quads per side at `density`. */
    static int grid_resolution_(const WaterBody& body, float density) {
        const int res = static_cast<int>(std::lround(static_cast<float>(body.resolution) * density));
        return std::clamp(res, 1, 512);
    }

    /** @brief The body's local-space triangle soup: its mesh source or a procedural grid of
     *         `grid_res` quads per side (set to -1 for mesh geometry). */
    static bool local_geometry_(const WaterBody& body, float density, std::vector<glm::vec3>& positions,
                                std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices, int& grid_res) {
        positions.clear();
        uvs.clear();
        indices.clear();
        grid_res = -1;
        if (!body.geometry_indices.empty()) {
            positions = body.geometry_positions;
            uvs       = body.geometry_uvs;
            uvs.resize(positions.size(), glm::vec2(0.0f));
            indices   = body.geometry_indices;
            return true;
        }
        if (!body.mesh_path.empty()) {
            if (!body.source.is_loaded()) return false;
            const auto& src = *body.source;
            positions.reserve(src.vertices.size());
            uvs.reserve(src.vertices.size());
            for (const auto& v : src.vertices) {
                positions.push_back(v.position);
                uvs.push_back(v.uv);
            }
            indices = src.indices;
            return !indices.empty();
        }
        const int res = grid_resolution_(body, density);
        grid_res = res;
        const glm::vec2 half = body.size * 0.5f;
        for (int y = 0; y <= res; ++y) {
            for (int x = 0; x <= res; ++x) {
                glm::vec2 f(static_cast<float>(x) / res, static_cast<float>(y) / res);
                positions.push_back(glm::vec3(-half + f * body.size, 0.0f));
                uvs.push_back(f);
            }
        }
        const uint32_t row = static_cast<uint32_t>(res + 1);
        for (uint32_t y = 0; y < static_cast<uint32_t>(res); ++y) {
            for (uint32_t x = 0; x < static_cast<uint32_t>(res); ++x) {
                uint32_t i0 = y * row + x, i1 = i0 + 1, i2 = i0 + row, i3 = i2 + 1;
                indices.insert(indices.end(), {i0, i1, i3, i0, i3, i2});
            }
        }
        return true;
    }

    /** @brief First STATIC, non-trigger hit along a ray; `out.normal` is the surface's own
     *         (outward / face) normal, NOT flipped toward the ray. */
    bool static_raycast_raw_(const glm::vec3& origin, const glm::vec3& dir, float max_distance,
                             FlowRayHit& out) const {
        if (!physics_) return false;
        coopa::physx::geometry::Ray ray;
        ray.origin = origin;
        ray.direction = dir;
        ray.max_distance = max_distance;
        const auto& world = physics_->world();
        // Fast path: the closest solid hit is almost always the static bank or bed. Only when a
        // dynamic body is in the way is every hit along the ray needed.
        coopa::physx::query::RaycastHit closest;
        if (!world.raycast(ray, closest, ~0u, false)) return false;
        if (const auto* b = world.get_body(closest.body); b && b->type == coopa::physx::dynamics::BodyType::Static) {
            out.distance = closest.distance;
            out.normal = closest.normal;
            return true;
        }
        for (const auto& hit : world.raycast_all(ray, ~0u, false)) {
            const auto* b = world.get_body(hit.body);
            if (!b || b->type != coopa::physx::dynamics::BodyType::Static) continue;
            out.distance = hit.distance;
            out.normal = hit.normal;
            return true;
        }
        return false;
    }

    /** @brief static_raycast_raw_() with the normal flipped to face back along the ray. */
    bool static_raycast_(const glm::vec3& origin, const glm::vec3& dir, float max_distance,
                         FlowRayHit& out) const {
        if (!static_raycast_raw_(origin, dir, max_distance, out)) return false;
        if (glm::dot(out.normal, dir) > 0.0f) out.normal = -out.normal;
        return true;
    }

    /** @brief Water depth below `p`: 0 when buried under a bank, max_depth over open water. */
    float probe_depth_(const glm::vec3& p, float max_depth) const {
        // Hitting an UP-facing surface on the way up means `p` is beneath ground (a water mesh
        // extending under its banks). A down-facing one (a pier deck's underside) is an
        // overhang, not burial.
        FlowRayHit up;
        if (static_raycast_raw_(p, glm::vec3(0.0f, 0.0f, 1.0f), 25.0f, up) && up.normal.z > 0.0f) {
            return 0.0f;
        }
        FlowRayHit down;
        if (static_raycast_raw_(p + glm::vec3(0.0f, 0.0f, 0.01f), glm::vec3(0.0f, 0.0f, -1.0f), max_depth, down)) {
            return std::max(down.distance - 0.01f, 0.0f);
        }
        return max_depth;
    }

    void bake_(coopa::scene::Scene& scene, WaterBody& body, const glm::mat4& world, bool with_physics) {
        std::vector<glm::vec3> corner_pos;
        std::vector<glm::vec2> corner_uv;
        std::vector<uint32_t>  corner_idx;
        int grid_res = -1;
        if (!local_geometry_(body, settings_.grid_density, corner_pos, corner_uv, corner_idx, grid_res)) return;

        std::vector<glm::vec3> local_pos;
        std::vector<glm::vec2> uvs;
        std::vector<uint32_t>  indices;
        if (grid_res >= 1) {
            // A procedural grid is already welded (and row-major, as build_grid_tiles() expects).
            local_pos = std::move(corner_pos);
            uvs       = std::move(corner_uv);
            indices   = std::move(corner_idx);
        } else {
            weld_by_position(corner_pos, corner_uv, corner_idx, local_pos, uvs, indices);
        }

        const std::size_t n = local_pos.size();
        std::vector<glm::vec3> world_pos(n);
        for (std::size_t i = 0; i < n; ++i) world_pos[i] = glm::vec3(world * glm::vec4(local_pos[i], 1.0f));

        std::vector<glm::vec3> flow(n, glm::vec3(0.0f));
        std::vector<float> turbulence(n, 0.0f);
        if (body.mode == WaterMode::Flowing) {
            FlowBakeParams fp;
            fp.speed           = body.flow_speed;
            fp.min_speed       = body.flow_min_speed;
            fp.slope_gain      = body.flow_slope_gain;
            fp.obstacle_radius = body.obstacle_radius;
            fp.wake_length     = body.wake_length;
            FlowRayFn ray;
            if (with_physics) {
                ray = [this](const glm::vec3& o, const glm::vec3& d, float m, FlowRayHit& h) {
                    return static_raycast_(o, d, m, h);
                };
            }
            bake_flow(world_pos, uvs, indices, fp, ray, flow, turbulence);
        }

        std::vector<float> depth(n, body.max_depth);
        if (with_physics) {
            // Two raycasts a vertex, independent of each other: spread over the job workers.
            // PhysicsWorld's queries are read-only and safe to run concurrently (thread_local
            // traversal stacks -- see broadphase/aabb_tree.h).
            auto probe = [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i) depth[i] = probe_depth_(world_pos[i], body.max_depth);
            };
            if (jobs_ && n >= 4096) jobs_->parallel_for_blocking(n, 0, probe);
            else probe(0, n);
        }

        // CPU query (world space).
        std::vector<WaterVertex> wv(n);
        for (std::size_t i = 0; i < n; ++i) wv[i] = {world_pos[i], flow[i], depth[i], turbulence[i]};
        body.query.build(std::move(wv), indices);

        publish_gpu_mesh_(scene, body, world, local_pos, indices, flow, depth, turbulence, grid_res);
        apply_material_(body);

        body.baked_world = world;
        body.baked_density = settings_.grid_density;
        body.baked = true;
        body.bake_stage = with_physics ? 2 : 1;
    }

    /** @brief The baked vertex stream (see the file doc's packing), object space. */
    static std::vector<coopa::gfx::engine::data::Vertex>
    make_vertices_(const glm::mat4& world, const std::vector<glm::vec3>& local_pos, const std::vector<uint32_t>& indices,
                   const std::vector<glm::vec3>& flow_ws, const std::vector<float>& depth,
                   const std::vector<float>& turbulence) {
        using coopa::gfx::engine::data::Vertex;
        // Object-space normals, area-weighted from the welded triangles.
        std::vector<glm::vec3> normals(local_pos.size(), glm::vec3(0.0f));
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            glm::vec3 a = local_pos[indices[t]], b = local_pos[indices[t + 1]], c = local_pos[indices[t + 2]];
            glm::vec3 cr = glm::cross(b - a, c - a);
            if (cr.z < 0.0f) cr = -cr; // winding is fixed per tile; the normal faces +Z regardless
            for (int k = 0; k < 3; ++k) normals[indices[t + k]] += cr;
        }
        const glm::mat3 to_local = glm::inverse(glm::mat3(world));
        std::vector<Vertex> verts(local_pos.size());
        for (std::size_t i = 0; i < local_pos.size(); ++i) {
            glm::vec3 nrm = glm::length(normals[i]) > 1e-12f ? glm::normalize(normals[i]) : glm::vec3(0.0f, 0.0f, 1.0f);
            if (nrm.z < 0.0f) nrm = -nrm;
            verts[i].position = local_pos[i];
            verts[i].normal   = nrm;
            verts[i].uv       = glm::vec2(depth[i], turbulence[i]);
            // w == 2: baked (see file doc). Never exactly zero: stock vertex shaders (the editor's
            // preview shading draws water with pbr.vert, not water.vert) normalize this slot as a
            // real tangent, and a zero vector would turn the whole surface NaN. 1e-4 m/s of
            // "flow" is invisible to water.vert and buoyancy alike.
            glm::vec3 t = to_local * flow_ws[i];
            if (glm::dot(t, t) < 1e-8f) t = glm::vec3(1e-4f, 0.0f, 0.0f);
            verts[i].tangent  = glm::vec4(t, 2.0f);
        }
        return verts;
    }

    /**
     * @brief Splits the baked surface into render tiles (water_tiles.h) and publishes them: one
     *        tile onto the owner's MeshRenderer, several as runtime child objects (see the file
     *        doc). `grid_res` >= 1 marks a row-major procedural grid of that many quads a side.
     */
    void publish_gpu_mesh_(coopa::scene::Scene& scene, WaterBody& body, const glm::mat4& world,
                           const std::vector<glm::vec3>& local_pos, const std::vector<uint32_t>& indices,
                           const std::vector<glm::vec3>& flow_ws, const std::vector<float>& depth,
                           const std::vector<float>& turbulence, int grid_res) {
        using coopa::gfx::engine::components::MeshRenderer;
        using coopa::gfx::engine::data::Mesh;
        if (!device_ || !allocator_ || !assets_ || !body.owner) return;

        const auto verts = make_vertices_(world, local_pos, indices, flow_ws, depth, turbulence);

        // Bounds inflation for what the vertex shader adds: the full wave sum, vertically and
        // horizontally (local space; a scaled body is rare and the margin is generous).
        const WaveSet& ws = body.wave_set();
        glm::vec3 inflate(0.05f);
        if (!ws.calm) {
            for (const auto& w : ws.waves) {
                inflate.x += w.qa;
                inflate.y += w.qa;
                inflate.z += w.amp;
            }
        }

        const glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                              glm::length(glm::vec3(world[2])));
        const float max_scale = std::max(std::max(scale.x, scale.y), std::max(scale.z, 1e-4f));
        // Auto: k_default_tile_size, but never more than k_max_tiles_per_side a side (each tile
        // has a per-frame CPU cost; see the constant's doc).
        float tile_len = body.tile_size;
        if (tile_len <= 0.0f) {
            glm::vec2 lo(1e30f), hi(-1e30f);
            for (const glm::vec3& p : local_pos) { lo = glm::min(lo, glm::vec2(p)); hi = glm::max(hi, glm::vec2(p)); }
            const float extent = std::max(hi.x - lo.x, hi.y - lo.y) * max_scale;
            tile_len = std::max(k_default_tile_size, extent / static_cast<float>(k_max_tiles_per_side));
        }
        tile_len /= max_scale;

        // LOD distances in the body's LOCAL units, like the tile bounds they are compared with:
        // the renderer's screen-size fraction (radius / distance) is then scale-independent.
        WaterTileLodParams lod;
        lod.lod_bias = settings_.lod_bias;
        if (!ws.calm) {
            for (int i = 0; i < k_wave_count && i < 4; ++i) {
                lod.wave_lambdas[i] = ws.waves[i].lambda / max_scale;
                lod.wave_amps[i]    = ws.waves[i].amp / max_scale;
            }
        }
        std::vector<WaterTile> tiles;
        const std::size_t grid_verts = grid_res >= 1 ? static_cast<std::size_t>(grid_res + 1) * (grid_res + 1) : 0;
        if (grid_res >= 1 && verts.size() == grid_verts) {
            const float spacing = std::max(body.size.x, body.size.y) / static_cast<float>(grid_res);
            int qpt = 1;
            while (qpt * 2 <= std::max(1.0f, tile_len / std::max(spacing, 1e-4f))) qpt *= 2;
            qpt = std::max(qpt, 2);
            // Rounding down to a power of two must not undo the auto cap on the tile count.
            if (body.tile_size <= 0.0f) {
                while ((grid_res + qpt - 1) / qpt > k_max_tiles_per_side) qpt *= 2;
            }
            tiles = build_grid_tiles(verts, grid_res, grid_res, qpt, spacing, lod, inflate);
        } else {
            tiles = build_mesh_tiles(verts, indices, tile_len, inflate, &lod);
        }
        if (tiles.empty()) return;

        auto* renderer = body.owner->get_component<MeshRenderer>();
        if (!renderer) renderer = add_default_renderer_(body);
        // One stable synthetic id per body instance (and tile): a re-bake (stage 2, a moved body,
        // an editor edit) re-publishes into the same slot, and AssetManager::create() retires the
        // previous mesh with its in-flight grace period -- frames still drawing it keep a valid
        // buffer.
        const std::string base = "runtime/water/" + body.owner->name() + "@" +
                                 std::to_string(reinterpret_cast<std::uintptr_t>(&body));

        if (tiles.size() == 1) {
            body.gpu_mesh = std::make_shared<Mesh>(Mesh::from_cpu(*device_, *allocator_, std::move(tiles[0].data)));
            body.single_asset_id = base;
            renderer->set_mesh(assets_->create<Mesh>(base, body.gpu_mesh));
            retire_tiles_(body, {});
            return;
        }

        // Several tiles: the owner keeps the authored material but draws nothing itself.
        body.gpu_mesh.reset();
        if (!body.single_asset_id.empty()) {
            renderer->set_mesh({});
            assets_->unload(coopa::asset::AssetId::from_path(body.single_asset_id));
            body.single_asset_id.clear();
        }
        std::vector<WaterBody::TileSlot> next;
        next.reserve(tiles.size());
        for (WaterTile& tile : tiles) {
            const std::string id = base + "/t" + std::to_string(tile.coord.x) + "_" + std::to_string(tile.coord.y);
            auto mesh = std::make_shared<Mesh>(Mesh::from_cpu(*device_, *allocator_, std::move(tile.data)));
            WaterBody::TileSlot slot;
            slot.asset_id = id;
            for (auto& old : body.tiles) {
                if (old.asset_id == id && old.object) {
                    slot.object = old.object;
                    old.object = nullptr; // reused: not retired below
                    break;
                }
            }
            if (!slot.object) {
                auto object = std::make_unique<coopa::scene::SceneObject>(
                    body.owner->name() + ".water_tile_" + std::to_string(tile.coord.x) + "_" + std::to_string(tile.coord.y));
                // Identity under the owner: tiles are in its local space. add_child() does NOT link
                // transforms (see SceneObject::add_child()); without this a tile would draw at the
                // world origin.
                auto* tc = object->add_component<coopa::scene::TransformComponent>();
                if (auto* owner_tc = body.owner->get_transform()) tc->set_parent_transform(&owner_tc->transform());
                auto* mr = object->add_component<MeshRenderer>();
                mr->material = renderer->material;
                mr->affects_reflection_probes = renderer->affects_reflection_probes;
                slot.object = body.owner->add_child(std::move(object));
                // Stamps the Scene back-pointer onto the new components, as TerrainSystem does.
                scene.adopt(*slot.object);
            }
            slot.object->get_component<MeshRenderer>()->set_mesh(assets_->create<Mesh>(id, std::move(mesh)));
            next.push_back(std::move(slot));
        }
        retire_tiles_(body, std::move(next));
    }

    /** @brief Destroys the tile objects of `body` not carried into `keep`, and makes `keep` its tiles. */
    void retire_tiles_(WaterBody& body, std::vector<WaterBody::TileSlot> keep) {
        for (auto& old : body.tiles) {
            if (!old.object) continue;
            // Destroying the object drops the last handle on its mesh, so the slot can go too.
            if (body.owner) body.owner->detach_child(old.object);
            if (assets_) assets_->unload(coopa::asset::AssetId::from_path(old.asset_id));
        }
        body.tiles = std::move(keep);
    }

    static coopa::gfx::engine::components::MeshRenderer* add_default_renderer_(WaterBody& body) {
        using namespace coopa::gfx::engine::components;
        auto* r = body.owner->add_component<MeshRenderer>();
        r->material.albedo               = glm::vec3(0.04f, 0.2f, 0.27f);
        r->material.alpha                = 0.55f;
        r->material.alpha_mode           = AlphaMode::Blend;
        r->material.roughness            = 0.06f;
        r->material.refraction           = true;
        r->material.ior                  = 1.33f;
        r->material.refraction_tint      = glm::vec3(0.55f, 0.82f, 0.86f);
        r->affects_reflection_probes     = false;
        r->scene                         = body.scene;
        return r;
    }

    static void apply_material_(WaterBody& body) {
        if (!body.owner) return;
        auto* renderer = body.owner->get_component<coopa::gfx::engine::components::MeshRenderer>();
        if (!renderer) return;
        auto& m = renderer->material;
        m.shader               = "water";
        m.shader_params        = body.waves.pack();
        m.shader_params_ext[0] = glm::vec4(body.foam_color, body.foam_amount);
        m.shader_params_ext[1] = glm::vec4(body.shore_foam_depth, body.edge_fade_depth,
                                           body.ripple_strength, body.ripple_scale);
        m.refraction_thickness = std::max(body.clarity, 1e-3f);
        if (m.alpha_mode != coopa::gfx::engine::components::AlphaMode::Blend) {
            m.alpha_mode = coopa::gfx::engine::components::AlphaMode::Blend;
        }
        sync_material(body);
    }

    // ------------------------------------------------------------------------------------
    // Buoyancy
    // ------------------------------------------------------------------------------------

    /** @brief Per-Rigidbody ripple bookkeeping, keyed by component, pruned every frame. */
    struct RippleEmitter {
        float     radius = -1.0f;      ///< Bounding radius about the COM; < 0 = not computed yet.
        bool      touching = false;    ///< Crossed the surface last frame.
        glm::vec2 last_position{0.0f}; ///< Where it last emitted.
        float     last_time = -1e9f;
        bool      seen = false;
    };

    /** @brief Bounding radius of a Rigidbody's (non-trigger) colliders about its centre of mass. */
    static float bounding_radius_(const coopa::physx::components::RigidbodyComponent& rb) {
        using coopa::physx::collision::ShapeType;
        float r = 0.0f;
        for (auto* col : rb.owner->get_components_in_children<coopa::physx::components::Collider>()) {
            if (col->is_trigger() || col->owner != rb.owner) continue; // children: rare, skip
            const glm::vec3 scale = coopa::physx::util::world_trs(col->owner->get_transform()->transform()).scale;
            const coopa::physx::collision::Shape s = col->make_shape(scale);
            const float c = glm::length(s.local_center - rb.local_center_of_mass());
            switch (s.type) {
                case ShapeType::Sphere:  r = std::max(r, c + s.radius); break;
                case ShapeType::Box:     r = std::max(r, c + glm::length(s.half_extents)); break;
                case ShapeType::Capsule: r = std::max(r, c + s.capsule_half_height + s.capsule_radius); break;
                default: break;
            }
        }
        return r;
    }

    /**
     * @brief Emits rings from every Rigidbody (dynamic or kinematic) crossing a water surface:
     *  - a SPLASH when it enters the water moving down fast,
     *  - a WAKE trail while it moves through the water (relative to the current, so a crate
     *    drifting with a river leaves none), one ring per spacing travelled,
     *  - a BOB ring now and then while it heaves up and down in place.
     * Strength scales with speed and size. A body at rest -- asleep, or floating still -- emits
     * nothing, so a calm lake goes quiet.
     */
    void emit_ripples_(coopa::scene::Scene& scene) {
        while (!ripples_.empty() && time_ - ripples_.front().birth > k_ripple_lifetime) {
            ripples_.erase(ripples_.begin());
        }
        if (bodies_.empty()) {
            emitters_.clear();
            return;
        }
        for (auto& [rb, e] : emitters_) e.seen = false;
        const float range2 = settings_.ripple_range * settings_.ripple_range;
        for (auto* rb : scene.get_components<coopa::physx::components::RigidbodyComponent>()) {
            if (!rb->owner || !rb->body_id().is_valid()) continue;
            RippleEmitter& e = emitters_[rb];
            e.seen = true;
            const glm::vec3 c = rb->world_center_of_mass();
            // Out of ripple range: too far for a ring to show. Checked before anything costs.
            if (has_focus_) {
                const glm::vec3 d = c - focus_;
                if (glm::dot(d, d) > range2) {
                    e.touching = false;
                    continue;
                }
            }
            if (e.radius < 0.0f) e.radius = bounding_radius_(*rb);
            if (e.radius <= 0.0f) continue;

            WaterSample s;
            const bool over = sample(glm::vec2(c), s);
            const bool touching = over && c.z - e.radius < s.surface_height && c.z + e.radius > s.surface_height;
            if (touching) {
                const glm::vec3 v = rb->velocity();
                const float vh = glm::length(glm::vec2(v) - glm::vec2(s.flow));
                const float spacing = std::max(0.3f, e.radius * 0.6f);
                if (!e.touching && v.z < -0.8f) {
                    emit_ripple(glm::vec2(c), glm::clamp(-v.z * e.radius * 0.35f, 0.1f, 1.5f), e.radius * 0.9f);
                } else if (vh > 0.25f && glm::distance(glm::vec2(c), e.last_position) > spacing) {
                    emit_ripple(glm::vec2(c), glm::clamp(vh * e.radius * 0.35f, 0.03f, 1.0f), e.radius * 0.6f);
                } else if (std::abs(v.z) > 0.25f && time_ - e.last_time > 0.5f) {
                    emit_ripple(glm::vec2(c), glm::clamp(std::abs(v.z) * e.radius * 0.3f, 0.02f, 0.6f), e.radius * 0.8f);
                } else {
                    e.touching = touching;
                    continue;
                }
                e.last_position = glm::vec2(c);
                e.last_time = time_;
            }
            e.touching = touching;
        }
        for (auto it = emitters_.begin(); it != emitters_.end();) {
            it = it->second.seen ? std::next(it) : emitters_.erase(it);
        }
    }

    struct BuoyantEntry {
        Buoyancy* buoyancy = nullptr;
        coopa::physx::components::RigidbodyComponent* rigidbody = nullptr;
        float bound_radius = 0.0f;
    };

    static float shape_volume_(const coopa::physx::collision::Shape& s) {
        using coopa::physx::collision::ShapeType;
        constexpr float pi = 3.14159265358979f;
        switch (s.type) {
            case ShapeType::Sphere:  return (4.0f / 3.0f) * pi * s.radius * s.radius * s.radius;
            case ShapeType::Box:     return 8.0f * s.half_extents.x * s.half_extents.y * s.half_extents.z;
            case ShapeType::Capsule: {
                float r = s.capsule_radius, hh = s.capsule_half_height;
                return pi * r * r * 2.0f * hh + (4.0f / 3.0f) * pi * r * r * r;
            }
            default: return 0.0f;
        }
    }

    /** @brief Element-wise |m|: maps a box's half extents to its rotated AABB half extents. */
    static glm::mat3 abs_mat3_(const glm::mat3& m) {
        return glm::mat3(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
    }

    /** @brief Builds Buoyancy::resolved from authored pontoons or the owner's colliders. */
    static void resolve_pontoons_(Buoyancy& b, int subdivision_cap) {
        using coopa::physx::collision::ShapeType;
        b.resolved.clear();
        coopa::scene::SceneObject* owner = b.owner;
        const glm::vec3 scale = coopa::physx::util::world_trs(owner->get_transform()->transform()).scale;

        std::vector<coopa::physx::collision::Shape> shapes;
        for (auto* col : owner->get_components_in_children<coopa::physx::components::Collider>()) {
            if (col->is_trigger()) continue;
            // Child colliders: express in the owner's frame (compound bodies).
            coopa::physx::collision::Shape s = col->make_shape(
                coopa::physx::util::world_trs(col->owner->get_transform()->transform()).scale);
            if (col->owner != owner) {
                auto root = coopa::physx::util::world_trs(owner->get_transform()->transform());
                auto child = coopa::physx::util::world_trs(col->owner->get_transform()->transform());
                glm::quat inv = glm::inverse(root.rotation);
                s.local_center   = inv * (child.position + child.rotation * s.local_center - root.position);
                s.local_rotation = inv * child.rotation * s.local_rotation;
            }
            if (s.type != ShapeType::TriangleMesh) shapes.push_back(s);
        }

        float total_volume = 0.0f;
        for (const auto& s : shapes) total_volume += shape_volume_(s);
        if (b.volume >= 0.0f) total_volume = b.volume;

        if (!b.pontoons.empty()) {
            const float v = total_volume / static_cast<float>(b.pontoons.size());
            for (const PontoonDesc& d : b.pontoons) {
                float r = d.radius * std::max(scale.x, std::max(scale.y, scale.z));
                b.resolved.push_back({d.position * scale, glm::vec3(r), v});
            }
            b.initialized = true;
            b.resolved_subdivision_cap = subdivision_cap;
            return;
        }

        const float geo_volume = std::max([&] {
            float sum = 0.0f;
            for (const auto& s : shapes) sum += shape_volume_(s);
            return sum;
        }(), 1e-6f);
        const float vol_scale = total_volume / geo_volume;

        for (const auto& s : shapes) {
            const float shape_vol = shape_volume_(s) * vol_scale;
            if (s.type == ShapeType::Box) {
                const int n = std::clamp(std::min(b.subdivisions, subdivision_cap), 1, 4);
                const glm::vec3 cell = s.half_extents * 2.0f / static_cast<float>(n);
                const float v = shape_vol / static_cast<float>(n * n * n);
                for (int z = 0; z < n; ++z)
                    for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x) {
                            glm::vec3 off = -s.half_extents + cell * (glm::vec3(x, y, z) + 0.5f);
                            // Half extents stay in the SHAPE's frame; rotated at sample time.
                            b.resolved.push_back({s.local_center + s.local_rotation * off,
                                                  abs_mat3_(glm::mat3_cast(s.local_rotation)) * (cell * 0.5f), v});
                        }
            } else if (s.type == ShapeType::Sphere) {
                // Equal-volume cube: side = r * cbrt(4 pi / 3).
                b.resolved.push_back({s.local_center, glm::vec3(s.radius * 0.806f), shape_vol});
            } else if (s.type == ShapeType::Capsule) {
                glm::vec3 axis(0.0f);
                axis[std::clamp(s.capsule_axis, 0, 2)] = 1.0f;
                const float len = 2.0f * (s.capsule_half_height + s.capsule_radius);
                const float seg = len / 3.0f;
                glm::vec3 he(s.capsule_radius * 0.886f);
                he[std::clamp(s.capsule_axis, 0, 2)] = seg * 0.5f;
                he = abs_mat3_(glm::mat3_cast(s.local_rotation)) * he;
                for (int i = -1; i <= 1; ++i) {
                    b.resolved.push_back({s.local_center + s.local_rotation * (axis * (seg * static_cast<float>(i))),
                                          he, shape_vol / 3.0f});
                }
            }
        }
        b.initialized = !b.resolved.empty();
        b.resolved_subdivision_cap = subdivision_cap;
    }

    void gather_buoyant_(coopa::scene::Scene& scene) {
        buoyant_.clear();
        const float enter = settings_.sim_radius;
        const float leave = settings_.sim_radius * settings_.sim_hysteresis;
        for (Buoyancy* b : scene.get_components<Buoyancy>()) {
            if (!b->owner) continue;
            auto* rb = b->owner->get_component<coopa::physx::components::RigidbodyComponent>();
            if (!rb || rb->is_kinematic) continue;
            if (b->initialized && b->resolved_subdivision_cap != settings_.max_box_subdivisions) b->initialized = false;
            if (!b->initialized) resolve_pontoons_(*b, settings_.max_box_subdivisions);
            if (!b->initialized) continue;
            BuoyantEntry e;
            e.buoyancy = b;
            e.rigidbody = rb;
            for (const Pontoon& p : b->resolved) {
                e.bound_radius = std::max(e.bound_radius,
                                          glm::length(p.local - rb->local_center_of_mass()) + glm::length(p.half_extents));
            }
            // Simulation range, with hysteresis so a body on the boundary doesn't flip each frame.
            if (has_focus_) {
                const float d = glm::distance(rb->world_center_of_mass(), focus_) - e.bound_radius;
                const bool was = b->simulated;
                b->simulated = d <= enter || (was && d <= leave);
                if (b->simulated != was) b->frozen_rest_time = 0.0f;
            } else {
                b->simulated = true;
            }
            buoyant_.push_back(e);
        }
    }

    void substep_(coopa::physx::PhysicsWorld& world, float h) {
        time_ += h;
        active_count_ = 0;
        if (bodies_.empty() || buoyant_.empty()) return;
        // active_bodies_ is gathered once per frame; a simulated floater may still be out of
        // reach of every one of them (then it is simply dry).

        const glm::vec3 gravity = world.gravity();
        const float g = std::max(-gravity.z, 0.0f);
        const WaveQueryOptions opt = wave_options_();

        for (BuoyantEntry& e : buoyant_) {
            Buoyancy& buoy = *e.buoyancy;
            auto* body = world.get_body(e.rigidbody->body_id());
            if (!body || body->type != coopa::physx::dynamics::BodyType::Dynamic) continue;
            if (!buoy.simulated) {
                frozen_substep_(e, *body, gravity, h);
                continue;
            }

            // Broad phase: which water bodies (near the focus) can this body touch at all?
            std::array<const WaterBody*, 4> cand{};
            int cand_n = 0;
            glm::vec2 lo = glm::vec2(body->position) - e.bound_radius;
            glm::vec2 hi = glm::vec2(body->position) + e.bound_radius;
            for (const WaterBody* wb : active_bodies_) {
                if (!wb->baked || !wb->query.overlaps_xy(lo, hi)) continue;
                if (body->position.z - e.bound_radius > wb->query.bounds_max().z + wb->waves.amplitude * 2.0f) continue;
                if (cand_n < 4) cand[cand_n++] = wb;
            }
            if (cand_n == 0) {
                buoy.in_water = false;
                buoy.submerged_fraction = 0.0f;
                continue;
            }

            // A sleeping body on still water stays asleep -- nothing would move it.
            if (!body->awake) {
                bool moving = false;
                for (int c = 0; c < cand_n; ++c) {
                    WaterSample s;
                    if (!cand[c]->waves.calm()) moving = true;
                    else if (cand[c]->sample(glm::vec2(body->position), time_, s, opt) && glm::length(s.flow) > 0.05f)
                        moving = true;
                }
                if (!moving) continue; // keeps its last in_water/submerged_fraction
                body->wake();
            }
            buoy.in_water = false;

            ++active_count_;
            const glm::mat3 rot = glm::mat3_cast(body->orientation);
            const glm::vec3 com_local = e.rigidbody->local_center_of_mass();
            const float m_share = body->mass / static_cast<float>(buoy.resolved.size());
            float vol_total = 0.0f, vol_sub = 0.0f;
            glm::vec3 flow_acc(0.0f);   // submersion-weighted current under the body
            float     area_sub = 0.0f;  // submersion-weighted frontal area, for form drag
            float     density_acc = 0.0f;
            const glm::vec3 up = g > 0.0f ? -gravity / g : glm::vec3(0.0f, 0.0f, 1.0f);

            for (const Pontoon& p : buoy.resolved) {
                const glm::vec3 wp = body->position + rot * (p.local - com_local);
                vol_total += p.volume;

                // Highest surface among candidate bodies (a river mouth overlapping its lake).
                WaterSample s;
                bool hit = false;
                float density = 1000.0f;
                for (int c = 0; c < cand_n; ++c) {
                    WaterSample sc;
                    if (!cand[c]->sample(glm::vec2(wp), time_, sc, opt)) continue;
                    if (!hit || sc.surface_height > s.surface_height) {
                        s = sc;
                        density = cand[c]->density;
                        hit = true;
                    }
                }
                if (!hit) continue;

                // Submersion ramps over the pontoon's body-frame vertical half extent. Constant on
                // purpose: an orientation-dependent ramp makes the buoyancy field non-conservative
                // (the force would depend on rotation with no matching torque), and that pumps
                // energy into a slow, never-damped rolling drift.
                const float r = std::max(p.half_extents.z, 1e-3f);
                const float f = glm::clamp((s.surface_height - (wp.z - r)) / (2.0f * r), 0.0f, 1.0f);
                if (f <= 0.0f) continue;
                buoy.in_water = true;
                vol_sub     += p.volume * f;
                flow_acc    += s.flow * (p.volume * f);
                density_acc += density * (p.volume * f);
                const float area = std::cbrt(p.volume * p.volume);
                area_sub    += area * f;

                // Archimedes, straight up (against gravity), plus the VERTICAL part of the drag,
                // both at the pontoon: off-centre application is what rights the body and damps
                // its pitch and roll. Drag is removed as a fraction of the relative velocity --
                // exponential for the linear term, clamped to 1 with the quadratic form drag
                // (0.5 rho Cd A |v|) -- so it is unconditionally stable and never reverses it.
                const float v_up = glm::dot(body->velocity_at_point(wp) - s.flow, up);
                const float quad = 0.5f * density * buoy.form_drag * area * f * std::abs(v_up) * h / std::max(m_share, 1e-4f);
                const float removed = std::min(1.0f, (1.0f - std::exp(-buoy.linear_drag * f * h)) + quad);
                const float j = density * g * p.volume * f * buoy.buoyancy_scale * h - v_up * removed * m_share;
                body->apply_impulse_at_position(up * j, wp, /*wake_body=*/false);
            }

            buoy.submerged_fraction = vol_total > 0.0f ? vol_sub / vol_total : 0.0f;
            if (vol_sub <= 0.0f) continue;

            // HORIZONTAL drag acts on the centre-of-mass velocity relative to the mean current
            // under the body, through the COM. Applied per pontoon instead, it is delivered
            // almost entirely below the COM of a floating body, couples sway into roll and yaw,
            // and leaves a slow rolling drift that never damps (and never lets a calm floater
            // sleep). What carries a body downstream is exactly this term.
            const float frac = buoy.submerged_fraction;
            const glm::vec3 flow = flow_acc / vol_sub;
            const float rho = density_acc / vol_sub;
            glm::vec3 v_rel = body->linear_velocity - flow;
            v_rel -= up * glm::dot(v_rel, up);
            const float quad = 0.5f * rho * buoy.form_drag * area_sub * glm::length(v_rel) * h / std::max(body->mass, 1e-4f);
            const float removed = std::min(1.0f, (1.0f - std::exp(-buoy.linear_drag * frac * h)) + quad);
            body->linear_velocity -= v_rel * removed;
            body->angular_velocity *= std::exp(-buoy.angular_drag * frac * h);
        }
    }

    /**
     * @brief A floater outside the simulation range. Asleep: nothing at all (it stays where it
     *        floated). Awake (knocked by something, or just left the range): cheap calm-water
     *        buoyancy -- one surface lookup under the centre of mass, no waves, no current --
     *        so it settles at its floating height instead of sinking, then is put to sleep.
     */
    void frozen_substep_(BuoyantEntry& e, coopa::physx::dynamics::Body& body, const glm::vec3& gravity, float h) {
        Buoyancy& buoy = *e.buoyancy;
        if (!body.awake) return; // keeps its last in_water/submerged_fraction
        const float g = std::max(-gravity.z, 0.0f);
        const glm::vec3 up = g > 0.0f ? -gravity / g : glm::vec3(0.0f, 0.0f, 1.0f);

        // The undisplaced surface under the centre of mass, any body (the frozen path is rare).
        WaterBaseSample base;
        bool hit = false;
        float density = 1000.0f;
        for (const WaterBody* wb : bodies_) {
            WaterBaseSample bs;
            if (!wb->baked || !wb->query.sample_base(glm::vec2(body.position), bs)) continue;
            if (!hit || bs.height > base.height) {
                base = bs;
                density = wb->density;
                hit = true;
            }
        }
        if (!hit) {
            buoy.in_water = false;
            buoy.submerged_fraction = 0.0f;
            buoy.frozen_rest_time = 0.0f;
            return;
        }

        // Vertical extent and volume from the pontoons, at the current orientation.
        const glm::mat3 rot = glm::mat3_cast(body.orientation);
        const glm::vec3 com_local = e.rigidbody->local_center_of_mass();
        float z_lo = 1e30f, z_hi = -1e30f, volume = 0.0f;
        for (const Pontoon& p : buoy.resolved) {
            const float z = body.position.z + (rot * (p.local - com_local)).z;
            const float r = std::max(p.half_extents.z, 1e-3f);
            z_lo = std::min(z_lo, z - r);
            z_hi = std::max(z_hi, z + r);
            volume += p.volume;
        }
        const float f = glm::clamp((base.height - z_lo) / std::max(z_hi - z_lo, 1e-3f), 0.0f, 1.0f);
        buoy.in_water = f > 0.0f;
        buoy.submerged_fraction = f;
        if (f > 0.0f) {
            // Archimedes through the centre of mass, and still-water drag on the whole velocity.
            body.linear_velocity += up * (density * g * volume * f * buoy.buoyancy_scale * h * body.inv_mass);
            const float removed = 1.0f - std::exp(-(buoy.linear_drag + 2.0f) * f * h);
            body.linear_velocity -= body.linear_velocity * removed;
            body.angular_velocity *= std::exp(-(buoy.angular_drag + 2.0f) * f * h);
        }

        // Settled: sleep. Velocity is zeroed (Body::sleep()); it resumes from here when woken.
        const bool resting = glm::length(body.linear_velocity) < 0.08f && glm::length(body.angular_velocity) < 0.08f;
        buoy.frozen_rest_time = resting ? buoy.frozen_rest_time + h : 0.0f;
        if (buoy.frozen_rest_time > 0.5f) {
            body.sleep();
            buoy.frozen_rest_time = 0.0f;
        }
    }

    coopa::job::JobEngine*          jobs_      = nullptr; ///< This frame's (FrameContext::jobs); may be null.
    coopa::gfx::core::Device*       device_    = nullptr;
    coopa::gfx::memory::Allocator*  allocator_ = nullptr;
    coopa::asset::AssetManager*     assets_    = nullptr;

    coopa::physx::system::PhysicsSystem* physics_ = nullptr;
    coopa::event::Connection substep_connection_;

    std::vector<WaterBody*> bodies_;
    std::vector<WaterBody*> active_bodies_; ///< Baked bodies within reach of the focus (per frame).
    std::vector<BuoyantEntry> buoyant_;
    std::vector<Ripple> ripples_;
    std::unordered_map<const coopa::physx::components::RigidbodyComponent*, RippleEmitter> emitters_;
    float       time_ = 0.0f;
    bool        simulating_ = false;   ///< Last execute()'s scene.is_simulating(), for render_time().
    std::size_t active_count_ = 0;

    WaterSettings settings_;
    glm::vec3     focus_{0.0f};
    bool          has_focus_ = false;
    bool          focus_override_ = false;
};

/**
 * @brief Constructs a WaterSystem and registers it just ahead of the physics phase.
 *        GPU arguments may be null for CPU-only (headless test) use.
 */
inline WaterSystem* install_water_system(coopa::scene::Scene& scene, coopa::gfx::core::Device* device = nullptr,
                                         coopa::gfx::memory::Allocator* allocator = nullptr,
                                         coopa::asset::AssetManager* assets = nullptr,
                                         int order = k_water_system_order) {
    auto sys = std::make_unique<WaterSystem>(device, allocator, assets);
    WaterSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_SYSTEM_H
