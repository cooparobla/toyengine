/**
 * @file fire_system.cpp
 * @brief Drives Burnable. See fire_system.h.
 */
#include <toyengine/fire/fire_system.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/particles/particle_system.h>
#include <toyengine/scene/runtime_object.h>


namespace toy {
namespace fire {
namespace {

using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using coopa::gfx::engine::components::MeshRenderer;
using toy::particles::ParticleSystem;

glm::mat4 world_of(const SceneObject* obj) {
    if (!obj) return glm::mat4(1.0f);
    const auto* tc = obj->get_transform();
    return tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
}

glm::vec3 world_pos(const SceneObject* obj) { return glm::vec3(world_of(obj)[3]); }



/** @brief Collects every MeshRenderer on `obj` and its descendants. */
void gather_renderers(SceneObject* obj, std::vector<MeshRenderer*>& out) {
    if (!obj) return;
    if (auto* mr = obj->get_component<MeshRenderer>()) out.push_back(mr);
    for (auto& c : obj->children()) gather_renderers(c.get(), out);
}

/** @brief Ray vs an axis-aligned box. Returns the near hit distance, or -1. */
float ray_box(const glm::vec3& o, const glm::vec3& d, const glm::vec3& lo, const glm::vec3& hi) {
    float tmin = 0.0f, tmax = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return -1.0f;
            continue;
        }
        const float inv = 1.0f / d[i];
        float t0 = (lo[i] - o[i]) * inv, t1 = (hi[i] - o[i]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmin > tmax) return -1.0f;
    }
    return tmin;
}

/** @brief The object's mesh bounds in its own local space, if it has a loaded mesh. */
bool local_bounds(SceneObject* obj, glm::vec3& lo, glm::vec3& hi) {
    std::vector<MeshRenderer*> rends;
    gather_renderers(obj, rends);
    bool any = false;
    const glm::mat4 inv_root = glm::inverse(world_of(obj));
    for (MeshRenderer* mr : rends) {
        const auto* mesh = mr->get_mesh().get();
        if (!mesh) continue;
        // A part's bounds are in ITS space; bring them into the root's so a multi-part object
        // (a chair's seat, back and legs) is one box.
        const glm::mat4 to_root = inv_root * world_of(mr->owner);
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? mesh->bounds_max().x : mesh->bounds_min().x,
                                   (c & 2) ? mesh->bounds_max().y : mesh->bounds_min().y,
                                   (c & 4) ? mesh->bounds_max().z : mesh->bounds_min().z);
            const glm::vec3 p = glm::vec3(to_root * glm::vec4(corner, 1.0f));
            lo = any ? glm::min(lo, p) : p;
            hi = any ? glm::max(hi, p) : p;
            any = true;
        }
    }
    return any;
}

std::unique_ptr<SceneObject> make_child(const std::string& name) {
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f));
    return obj;
}

/**
 * @brief Adds `child` to `parent` AND links their transforms. Returns the adopted child.
 *
 * `SceneObject::add_child` deliberately does not touch transforms -- the scene loader links
 * them explicitly as it builds a hierarchy. A subtree built at RUNTIME therefore has unparented
 * transforms, and every object in it sits at the world origin however its parent is placed.
 */
SceneObject* attach_child(SceneObject& parent, std::unique_ptr<SceneObject> child) {
    SceneObject* raw = parent.add_child(std::move(child));
    if (auto* child_tc = raw->get_transform()) {
        if (auto* parent_tc = parent.get_transform()) {
            child_tc->set_parent_transform(&parent_tc->transform());
        }
    }
    return raw;
}

} // namespace






void FireSystem::set_material_burn_(SceneObject* obj, bool burning, const glm::vec4& front) {
    std::vector<MeshRenderer*> rends;
    gather_renderers(obj, rends);
    for (MeshRenderer* mr : rends) {
        mr->material.burning = burning;
        if (burning) mr->material.shader_params = front;
    }
}


std::shared_ptr<const particles::MeshSurface> FireSystem::bake_surface_(SceneObject* obj) {
    if (!surface_loader_ || !obj) return nullptr;
    std::vector<MeshRenderer*> rends;
    gather_renderers(obj, rends);

    // One surface for the WHOLE object, in the object's own space. Per-part emitters would
    // mean a dozen particle systems on a chair and a band test repeated for each; merging
    // once at ignition means a single emitter whose samples already span every part, and the
    // band then picks out the line crossing all of them at once.
    std::vector<glm::vec3> pos, nrm, tan;
    const glm::mat4 inv_root = glm::inverse(world_of(obj));
    for (MeshRenderer* mr : rends) {
        if (!mr->owner || mr->mesh_path().empty()) continue;
        std::shared_ptr<const particles::MeshSurface> part = surface_loader_(mr->mesh_path());
        if (!part || part->empty()) continue;
        const glm::mat4 to_root = inv_root * world_of(mr->owner);
        const glm::mat3 normal_m = glm::transpose(glm::inverse(glm::mat3(to_root)));
        // export_faces() is the only way back to the triangles: 8 vec4 per triangle, p0/p1/p2
        // then n0/n1/n2, then the face normal and a tangent.
        const std::vector<glm::vec4> faces = part->export_faces();
        for (std::size_t f = 0; f + 7 < faces.size(); f += 8) {
            for (int c = 0; c < 3; ++c) {
                pos.push_back(glm::vec3(to_root * glm::vec4(glm::vec3(faces[f + c]), 1.0f)));
                nrm.push_back(glm::normalize(normal_m * glm::vec3(faces[f + 3 + c])));
                tan.push_back(glm::vec3(0.0f));
            }
        }
    }
    if (pos.empty()) return nullptr;
    auto merged = std::make_shared<particles::MeshSurface>();
    merged->build(pos, nrm, tan, {});
    return merged;
}

SceneObject* FireSystem::make_burn_effects_(coopa::scene::Scene& scene, Burnable& b) {
    std::shared_ptr<const particles::MeshSurface> surface = bake_surface_(b.owner);
    if (!surface) return nullptr;
    if (!root_) {
        root_ = toy::scene::spawn_runtime_root(scene, "Fire", "Fire",
                                               "Smoke and flame on the burn line of whatever is "
                                               "currently alight");
    }
    if (!root_) return nullptr;

    // Parented to the BURNING OBJECT, not to the runtime root: the emitters sample that
    // object's triangles in its own space, so they have to share its transform or every
    // particle comes out in the wrong place.
    SceneObject* h = attach_child(*b.owner, make_child(b.owner->name() + "_burnline"));

    auto add = [&](const char* name) -> ParticleSystem* {
        auto o = make_child(name);
        auto* ps = o->add_component<ParticleSystem>();
        ps->settings.shape.type = particles::EmitShape::Mesh;
        ps->settings.shape.emit_from = particles::MeshEmitFrom::Faces;
        ps->settings.shape.normal_offset = 0.012f;
        ps->set_shape_surface(surface);
        attach_child(*h, std::move(o));
        return ps;
    };

    // Flame: small, short-lived, hugging the surface.
    {
        auto& st = add("flames")->settings;
        st.rate = 26.0f;
        st.start_lifetime = particles::Range(0.18f, 0.38f);
        st.start_speed = particles::Range(0.12f, 0.4f);
        st.start_size = particles::Range(0.03f, 0.075f);
        st.gravity = -0.5f;
        st.drag = 2.4f;
        st.size_over_life = particles::FloatCurve({{0.0f, 0.5f}, {0.25f, 1.0f}, {1.0f, 0.1f}});
        st.alpha_over_life = particles::FloatCurve({{0.0f, 0.8f}, {1.0f, 0.0f}});
        st.look.mode = render::ParticleRenderMode::Vertical;
        st.look.sprite = render::ParticleSprite::Flame;
        st.look.lit = 0.0f;
        st.look.additive = 0.5f;
        st.look.emissive = 1.3f;
        st.look.pivot_z = 0.42f;
        st.color_over_life = particles::Gradient({
            {0.0f, glm::vec4(1.0f, 0.86f, 0.45f, 0.9f)},
            {0.45f, glm::vec4(1.0f, 0.42f, 0.08f, 0.85f)},
            {1.0f, glm::vec4(0.35f, 0.08f, 0.02f, 0.0f)},
        });
    }
    // Smoulder: sparse embers that lift off the line and die quickly.
    {
        auto& st = add("smoulder")->settings;
        st.rate = 7.0f;
        st.start_lifetime = particles::Range(0.5f, 1.3f);
        st.start_speed = particles::Range(0.15f, 0.55f);
        st.start_size = particles::Range(0.006f, 0.014f);
        st.gravity = -0.3f;
        st.wind_influence = 0.8f;
        st.noise_strength = 2.2f;
        st.size_over_life = particles::FloatCurve({{0.0f, 1.0f}, {1.0f, 0.2f}});
        st.look.mode = render::ParticleRenderMode::Stretched;
        st.look.sprite = render::ParticleSprite::Spark;
        st.look.lit = 0.0f;
        st.look.additive = 1.0f;
        st.look.emissive = 1.9f;
        st.look.reactive = 0.6f;
        st.color_over_life = particles::Gradient({
            {0.0f, glm::vec4(1.0f, 0.55f, 0.12f, 1.0f)},
            {1.0f, glm::vec4(0.5f, 0.09f, 0.02f, 0.0f)},
        });
    }
    // Smoke: the thing that actually reads at a distance.
    {
        auto& st = add("smoke")->settings;
        st.rate = 11.0f;
        st.start_lifetime = particles::Range(1.6f, 3.0f);
        st.start_speed = particles::Range(0.18f, 0.45f);
        st.start_size = particles::Range(0.07f, 0.14f);
        st.gravity = -0.12f;
        st.drag = 0.4f;
        st.wind_influence = 0.7f;
        st.start_rotation = particles::Range(0.0f, 360.0f);
        st.size_over_life = particles::FloatCurve({{0.0f, 0.4f}, {1.0f, 2.4f}});
        st.look.sprite = render::ParticleSprite::Puff;
        st.look.lit = 0.9f;
        st.look.additive = 0.0f;
        st.color_over_life = particles::Gradient({
            {0.0f, glm::vec4(0.24f, 0.22f, 0.21f, 0.0f)},
            {0.25f, glm::vec4(0.22f, 0.21f, 0.20f, 0.42f)},
            {1.0f, glm::vec4(0.3f, 0.29f, 0.28f, 0.0f)},
        });
    }

    scene.adopt(*h);
    h->start();
    return h;
}

void FireSystem::drop_burn_effects_(Burnable& b) {
    if (!b.effects_root_) return;
    if (SceneObject* parent = b.effects_root_->parent()) (void)parent->detach_child(b.effects_root_);
    b.effects_root_ = nullptr;
}

void FireSystem::step_burnables_(coopa::scene::Scene& scene, float dt) {
    for (Burnable* b : scene.get_components<Burnable>()) {
        if (!b->owner) continue;
        if (!b->burning_) {
            // Spent objects keep their char: the front stays where it stopped so the surface
            // layer still has something to draw, but nothing is alight any more.
            if (!b->spent_) set_material_burn_(b->owner, false, glm::vec4(0.0f));
            drop_burn_effects_(*b);
            continue;
        }

        b->age_ += dt;
        b->front_ += b->spread_rate * dt;

        const glm::vec3 origin = b->origin_world();
        set_material_burn_(b->owner, true, glm::vec4(origin, b->front_));

        if (!b->effects_root_) b->effects_root_ = make_burn_effects_(scene, *b);
        if (b->effects_root_) {
            // The band rides the front, in the object's own space -- the space its triangles
            // are sampled in. A uniformly scaled object needs the radius brought into that
            // space too; a non-uniformly scaled one would need the band to be an ellipsoid,
            // which it is not, so the scale is taken as its largest axis.
            const glm::mat4 w = world_of(b->owner);
            const float scale = std::max({glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])),
                                          glm::length(glm::vec3(w[2])), 1e-4f});
            for (auto& child : b->effects_root_->children()) {
                if (auto* ps = child->get_component<ParticleSystem>()) {
                    ps->settings.shape.band_center = b->origin_local_;
                    ps->settings.shape.band_radius = b->front_ / scale;
                    ps->settings.shape.band_width = 0.1f / scale;
                }
            }
        }

        // Deliberately NO emitters. A burning object's fire is drawn by the burn layer, along
        // the edge between burnt and unburnt where it actually is. Hanging a particle system off
        // the ignition point instead threw flames radially out into the air around the object,
        // which is the one thing a spreading burn does not do. A purely visual fire -- a
        // candle, a campfire -- is a particle PREFAB, not anything this system builds.
        ++burning_count_;

        if (b->age_ >= b->burn_time) finish_burn_(*b);
    }
}

void FireSystem::finish_burn_(Burnable& b) {
    b.burning_ = false;
    b.spent_ = true;
    drop_burn_effects_(b);
    // Leave the front wide open so the whole object reads as charred, and keep `burning` set:
    // the surface layer is what draws the char, and a spent object is still burnt.
    glm::vec3 lo(0.0f), hi(0.0f);
    float reach = 10.0f;
    if (local_bounds(b.owner, lo, hi)) reach = glm::length(hi - lo) + b.overshoot;
    set_material_burn_(b.owner, b.on_burnt == BurntResult::Char,
                       glm::vec4(b.origin_world(), reach));
    if (b.on_burnt == BurntResult::Remove && b.owner) b.owner->set_active(false);
}

Burnable* FireSystem::ignite_ray(coopa::scene::Scene& scene, const glm::vec3& origin,
                                 const glm::vec3& direction, float max_distance) {
    const glm::vec3 dir = glm::normalize(direction);
    Burnable* best = nullptr;
    float best_t = max_distance;
    glm::vec3 best_point(0.0f);
    std::vector<MeshRenderer*> rends;

    for (Burnable* b : scene.get_components<Burnable>()) {
        if (!b->owner || b->spent_) continue;
        // Test each PART's own box, not one box around the whole object. A chair is mostly
        // empty space inside its bounds: a single box reports a click aimed at the seat as a
        // hit on the lid 60 cm above it, and the fire then starts in mid-air.
        rends.clear();
        gather_renderers(b->owner, rends);
        for (MeshRenderer* mr : rends) {
            const auto* mesh = mr->get_mesh().get();
            if (!mesh || !mr->owner) continue;
            const glm::mat4 part = world_of(mr->owner);
            const glm::mat4 inv = glm::inverse(part);
            const glm::vec3 ro = glm::vec3(inv * glm::vec4(origin, 1.0f));
            const glm::vec3 rd = glm::vec3(inv * glm::vec4(dir, 0.0f));
            const float t = ray_box(ro, rd, mesh->bounds_min(), mesh->bounds_max());
            if (t < 0.0f) continue;
            const glm::vec3 hit_world = glm::vec3(part * glm::vec4(ro + rd * t, 1.0f));
            const float dist = glm::length(hit_world - origin);
            if (dist < best_t) { best_t = dist; best = b; best_point = hit_world; }
        }
    }
    if (best) best->ignite_at(best_point);
    return best;
}

void FireSystem::reset(coopa::scene::Scene& scene) {
    for (Burnable* b : scene.get_components<Burnable>()) {
        drop_burn_effects_(*b);
        if (b->owner) {
            b->owner->set_active(true);
            set_material_burn_(b->owner, false, glm::vec4(0.0f));
        }
        b->reset();
    }
}

void FireSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    const float dt = std::max(ctx.delta_time, 0.0f);
    burning_count_ = 0;

    // Click-to-ignite. Engine::drive_fire_igniters_() has already put this frame's pick ray
    // and button state on each FireIgniter -- the same push-not-pull contract FreeMover uses.
    for (FireIgniter* g : scene.get_components<FireIgniter>()) {
        if (g->reset_requested) reset(scene);
        if (g->ignite_requested) {
            ignite_ray(scene, g->ray_origin, g->ray_direction, g->max_distance);
        }
        g->ignite_requested = false;
        g->reset_requested = false;
    }

    if (scene.is_simulating()) step_burnables_(scene, dt);
}

FireSystem* install_fire_system(coopa::scene::Scene& scene, int order) {
    if (FireSystem* existing = find_fire(scene)) return existing;
    auto sys = std::make_unique<FireSystem>();
    FireSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

FireSystem* find_fire(coopa::scene::Scene& scene) {
    return dynamic_cast<FireSystem*>(scene.find_system("Fire"));
}

} // namespace fire
} // namespace toy
