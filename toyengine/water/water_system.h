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
 *  1. As soon as its geometry exists -- so frame 0 already draws water -- with whatever physics
 *     knows. On the very first frame PhysicsSystem has not gathered colliders yet (it runs after
 *     this system), so that bake has no depth or obstacle information.
 *  2. Once PhysicsSystem has run once, again WITH raycasts against static colliders: per-vertex
 *     water depth (calms waves toward the shore, see water_waves.h) and, for flowing water,
 *     obstacle deflection and wakes (water_flow_bake.h). The mesh is re-uploaded once.
 * A body whose Transform moves is re-baked (water bodies are expected to be static; this keeps
 * an editor drag correct rather than fast).
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
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>

#include <physxcoopa/components/collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_flow_bake.h>
#include <toyengine/water/water_surface_query.h>
#include <toyengine/water/water_waves.h>

namespace toy {
namespace water {

/** @brief Default registration order: just ahead of UpdatePhase::Physics (100). See file doc. */
inline constexpr int k_water_system_order = 90;

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

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        connect_physics_(scene);
        if (!physics_) time_ += ctx.delta_time; // otherwise advanced per substep, in lockstep

        // Physics has completed at least one execute() once we see it here a second time.
        const bool physics_ready = physics_ != nullptr && frames_seen_ > 0;
        ++frames_seen_;

        bodies_ = scene.get_components<WaterBody>();
        for (WaterBody* body : bodies_) {
            if (!body->owner) continue;
            const glm::mat4& world = body->owner->get_transform()->transform().get_world_matrix();
            const bool moved = body->baked && world != body->baked_world;
            const bool needs_stage2 = body->baked && physics_ready && body->bake_stage < 2;
            if (body->baked && !moved && !needs_stage2) continue;
            bake_(*body, world, physics_ready);
        }

        gather_buoyant_(scene);
        emit_ripples_(scene);
    }

    /** @brief Water clock (seconds) -- the time every wave on the CPU is evaluated at. */
    float time() const { return time_; }

    /**
     * @brief Samples the highest water surface above world XY `p` across every water body.
     * @param which Optional: the body that answered.
     * @return false if no body covers `p`.
     */
    bool sample(const glm::vec2& p, WaterSample& out, const WaterBody** which = nullptr) const {
        bool found = false;
        for (const WaterBody* body : bodies_) {
            WaterSample s;
            if (!body->sample(p, time_, s)) continue;
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
    /// Live rings are capped (oldest dropped first): the shader loops over all of them.
    static constexpr std::size_t k_max_ripples = 64;
    /// Seconds a ring lives; its amplitude has decayed to ~2% by then.
    static constexpr float k_ripple_lifetime = 3.0f;

    /** @brief Live rings, oldest first. */
    const std::vector<Ripple>& ripples() const { return ripples_; }

    /** @brief Adds a ring by hand (gameplay: a thrown stone, a footstep). */
    void emit_ripple(const glm::vec2& position, float strength, float radius) {
        ripples_.push_back({position, time_, strength, radius});
        if (ripples_.size() > k_max_ripples) ripples_.erase(ripples_.begin());
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
    // Bake
    // ------------------------------------------------------------------------------------

    /** @brief The body's local-space triangle soup: its mesh source or a procedural grid. */
    static bool local_geometry_(const WaterBody& body, std::vector<glm::vec3>& positions,
                                std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices) {
        positions.clear();
        uvs.clear();
        indices.clear();
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
        const int res = std::clamp(body.resolution, 1, 512);
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

    void bake_(WaterBody& body, const glm::mat4& world, bool with_physics) {
        std::vector<glm::vec3> corner_pos;
        std::vector<glm::vec2> corner_uv;
        std::vector<uint32_t>  corner_idx;
        if (!local_geometry_(body, corner_pos, corner_uv, corner_idx)) return;

        std::vector<glm::vec3> local_pos;
        std::vector<glm::vec2> uvs;
        std::vector<uint32_t>  indices;
        weld_by_position(corner_pos, corner_uv, corner_idx, local_pos, uvs, indices);

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
            for (std::size_t i = 0; i < n; ++i) depth[i] = probe_depth_(world_pos[i], body.max_depth);
        }

        // CPU query (world space).
        std::vector<WaterVertex> wv(n);
        for (std::size_t i = 0; i < n; ++i) wv[i] = {world_pos[i], flow[i], depth[i], turbulence[i]};
        body.query.build(std::move(wv), indices);

        publish_gpu_mesh_(body, world, local_pos, indices, flow, depth, turbulence);
        apply_material_(body);

        body.baked_world = world;
        body.baked = true;
        body.bake_stage = with_physics ? 2 : 1;
    }

    void publish_gpu_mesh_(WaterBody& body, const glm::mat4& world, const std::vector<glm::vec3>& local_pos,
                           const std::vector<uint32_t>& indices, const std::vector<glm::vec3>& flow_ws,
                           const std::vector<float>& depth, const std::vector<float>& turbulence) {
        using coopa::gfx::engine::data::Mesh;
        using coopa::gfx::engine::data::Vertex;
        if (!device_ || !allocator_ || !assets_ || !body.owner) return;

        // Object-space normals, area-weighted from the welded triangles.
        std::vector<glm::vec3> normals(local_pos.size(), glm::vec3(0.0f));
        for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
            glm::vec3 a = local_pos[indices[t]], b = local_pos[indices[t + 1]], c = local_pos[indices[t + 2]];
            glm::vec3 cr = glm::cross(b - a, c - a);
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
            verts[i].tangent  = glm::vec4(to_local * flow_ws[i], 2.0f); // w == 2: baked (see file doc)
        }
        // Triangles wound so the normal faces +Z (TransparentPass culls back faces).
        std::vector<uint32_t> idx = indices;
        for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
            glm::vec3 a = local_pos[idx[t]], b = local_pos[idx[t + 1]], c = local_pos[idx[t + 2]];
            if (glm::cross(b - a, c - a).z < 0.0f) std::swap(idx[t + 1], idx[t + 2]);
        }

        auto* renderer = body.owner->get_component<coopa::gfx::engine::components::MeshRenderer>();
        if (!renderer) renderer = add_default_renderer_(body);

        body.gpu_mesh = std::make_shared<Mesh>(Mesh::from_arrays(*device_, *allocator_, verts, idx, 1));
        // A fresh synthetic id per bake: AssetManager publishes ids once, and the previous handle
        // is released (and its mesh freed) when set_mesh() replaces it.
        const std::string id = "runtime/water/" + body.owner->name() + "#" + std::to_string(++publish_serial_);
        renderer->set_mesh(assets_->create<Mesh>(id, body.gpu_mesh));
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
        for (auto* rb : scene.get_components<coopa::physx::components::RigidbodyComponent>()) {
            if (!rb->owner || !rb->body_id().is_valid()) continue;
            RippleEmitter& e = emitters_[rb];
            e.seen = true;
            if (e.radius < 0.0f) e.radius = bounding_radius_(*rb);
            if (e.radius <= 0.0f) continue;

            const glm::vec3 c = rb->world_center_of_mass();
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
    static void resolve_pontoons_(Buoyancy& b) {
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
                const int n = std::clamp(b.subdivisions, 1, 4);
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
    }

    void gather_buoyant_(coopa::scene::Scene& scene) {
        buoyant_.clear();
        for (Buoyancy* b : scene.get_components<Buoyancy>()) {
            if (!b->owner) continue;
            auto* rb = b->owner->get_component<coopa::physx::components::RigidbodyComponent>();
            if (!rb || rb->is_kinematic) continue;
            if (!b->initialized) resolve_pontoons_(*b);
            if (!b->initialized) continue;
            BuoyantEntry e;
            e.buoyancy = b;
            e.rigidbody = rb;
            for (const Pontoon& p : b->resolved) {
                e.bound_radius = std::max(e.bound_radius,
                                          glm::length(p.local - rb->local_center_of_mass()) + glm::length(p.half_extents));
            }
            buoyant_.push_back(e);
        }
    }

    void substep_(coopa::physx::PhysicsWorld& world, float h) {
        time_ += h;
        active_count_ = 0;
        if (bodies_.empty() || buoyant_.empty()) return;

        const glm::vec3 gravity = world.gravity();
        const float g = std::max(-gravity.z, 0.0f);

        for (BuoyantEntry& e : buoyant_) {
            Buoyancy& buoy = *e.buoyancy;
            auto* body = world.get_body(e.rigidbody->body_id());
            if (!body || body->type != coopa::physx::dynamics::BodyType::Dynamic) continue;

            // Broad phase: which water bodies can this body touch at all?
            std::array<const WaterBody*, 4> cand{};
            int cand_n = 0;
            glm::vec2 lo = glm::vec2(body->position) - e.bound_radius;
            glm::vec2 hi = glm::vec2(body->position) + e.bound_radius;
            for (const WaterBody* wb : bodies_) {
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
                    else if (cand[c]->sample(glm::vec2(body->position), time_, s) && glm::length(s.flow) > 0.05f)
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
                    if (!cand[c]->sample(glm::vec2(wp), time_, sc)) continue;
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

    coopa::gfx::core::Device*       device_    = nullptr;
    coopa::gfx::memory::Allocator*  allocator_ = nullptr;
    coopa::asset::AssetManager*     assets_    = nullptr;

    coopa::physx::system::PhysicsSystem* physics_ = nullptr;
    coopa::event::Connection substep_connection_;

    std::vector<WaterBody*> bodies_;
    std::vector<BuoyantEntry> buoyant_;
    std::vector<Ripple> ripples_;
    std::unordered_map<const coopa::physx::components::RigidbodyComponent*, RippleEmitter> emitters_;
    float       time_ = 0.0f;
    uint64_t    frames_seen_ = 0;
    uint64_t    publish_serial_ = 0;
    std::size_t active_count_ = 0;
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
