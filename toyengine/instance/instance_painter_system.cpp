#include <toyengine/instance/instance_painter_system.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include <coopa/scene/scene.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/mesh_collider.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/particles/terrain_scatter.h>
#include <toyengine/scene/runtime_object.h>

namespace toy {
namespace instance {

namespace {

using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

/// Lines already logged, so a broken prototype warns once rather than once a frame.
std::set<std::string>& logged_lines() {
    static std::set<std::string> lines;
    return lines;
}

/** @brief A painter's batch-key seed, from its SET (stable across component rebuilds). */
std::uint64_t painter_key_of(const InstancePainter& p) {
    const std::uint64_t uid = p.data ? p.data->uid() : 1ull;
    return (uid * 0x9E3779B97F4A7C15ull) | 1ull;
}

std::uint64_t batch_key(std::uint64_t painter, ChunkCoord c, std::uint16_t proto, std::uint32_t part) {
    const std::uint64_t coord = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) << 32) |
                                static_cast<std::uint32_t>(c.y);
    std::uint64_t k = painter ^ (coord * 0xC2B2AE3D27D4EB4Full) ^
                      ((static_cast<std::uint64_t>(proto) << 40) + (static_cast<std::uint64_t>(part) << 24) + 1ull);
    return k == 0 ? 1ull : k;
}

} // namespace

void InstancePainterSystem::log_once_(const std::string& line) {
    if (!logged_lines().insert(line).second) return;
    std::cerr << line << "\n";
}

void InstancePainterSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) {
    const auto painters = scene.get_components<InstancePainter>();
    physics_dirty_ = false;

    // Painters that went away take their caches with them. Identity is the instance set's uid,
    // never the component pointer: the editor rebuilds the component on every keystroke and
    // reuses its address. Nothing dereferences a key -- a painter disappears when its object is
    // destroyed, and the collider children hanging off that object go with it.
    std::set<std::uint64_t> live;
    for (const InstancePainter* p : painters) {
        if (p != nullptr && p->data) live.insert(p->data->uid());
    }
    for (auto it = painted_.begin(); it != painted_.end();) {
        if (live.find(it->first) == live.end()) {
            if (!it->second.collision.empty()) physics_dirty_ = true;
            it = painted_.erase(it);
        } else {
            ++it;
        }
    }

    const glm::vec3 camera = [&]() {
        using coopa::gfx::engine::components::CameraComponent;
        const CameraComponent* cam = CameraComponent::main();
        return cam != nullptr ? cam->get_world_position() : glm::vec3(0.0f);
    }();

    for (InstancePainter* painter : painters) {
        Painted* p = painted_for_(*painter);
        if (p == nullptr) continue;
        update_painter_(scene, *painter, *p, camera);
    }
    if (physics_dirty_) {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) {
            physics->refresh();
        }
    }
}

InstancePainterSystem::Painted* InstancePainterSystem::painted_for_(InstancePainter& painter) {
    if (!painter.data) return nullptr;
    Painted& p = painted_[painter.data->uid()];
    // Derived from the SET, so the renderer's persistent buffers survive a component rebuild
    // instead of being retired and re-uploaded on every inspector keystroke.
    if (p.key == 0) p.key = painter_key_of(painter);
    return &p;
}

void InstancePainterSystem::update_painter_(coopa::scene::Scene& scene, InstancePainter& painter, Painted& p,
                                            const glm::vec3& camera) {
    if (!painter.enabled) {
        p.cache.clear();
        drop_collision_(painter, p);
        return;
    }
    // The painter's chunk size is authored on the component; the data is the thing that routes
    // by it, so keep the two in step even when the inspector changed it after the parse.
    if (painter.data->chunk_size() != painter.chunk_size) painter.data->set_chunk_size(painter.chunk_size);

    float max_distance = 0.0f;
    bool all_bounds = true;
    for (InstancePrototype& proto : painter.prototypes) {
        resolve_prototype(proto, [this](const std::string& line) { log_once_(line); });
        if (!proto.ready()) {
            all_bounds = false;
            continue;
        }
        // Bounds need the meshes, which load asynchronously: fill them in on the frame they land.
        if (proto.bounds_min == proto.bounds_max) refresh_prototype_bounds(proto);
        const float d = proto.draw_distance > 0.0f ? proto.draw_distance
                                                   : std::clamp(150.0f * proto.nominal_height(), 40.0f, 2000.0f);
        max_distance = std::max(max_distance, d);
    }
    p.bounds_ready = all_bounds;

    p.cache.update(*painter.data, painter.prototypes, camera, max_distance, 2);
    update_collision_(scene, painter, p, camera);
}

// --- collision ---

coopa::scene::SceneObject* InstancePainterSystem::build_collision_chunk_(InstancePainter& painter, ChunkCoord coord) {
    using namespace coopa::physx::components;
    const std::vector<InstanceRecord>* records = painter.data->records(coord);
    if (records == nullptr || records->empty()) return nullptr;

    std::unique_ptr<SceneObject> parent;
    for (const InstanceRecord& r : *records) {
        if (r.hidden() || r.prototype >= painter.prototypes.size()) continue;
        const InstancePrototype& proto = painter.prototypes[r.prototype];
        if (proto.collision == InstanceCollision::None || !proto.ready()) continue;

        const glm::vec3 size = proto.collision_size != glm::vec3(0.0f) ? proto.collision_size
                                                                       : proto.bounds_max - proto.bounds_min;
        const glm::vec3 centre = 0.5f * (proto.bounds_min + proto.bounds_max);
        const float width = std::max(size.x, size.y);
        InstanceCollision shape = proto.collision;
        if (shape == InstanceCollision::Auto) {
            // Taller than it is wide: a capsule stands a trunk up. Otherwise a box, which rests
            // a boulder or a crate on its face instead of balancing it on a sphere.
            shape = size.z > 1.5f * width ? InstanceCollision::Capsule : InstanceCollision::Box;
        }

        if (parent == nullptr) {
            parent = std::make_unique<SceneObject>("instance_collision_" + std::to_string(coord.x) + "_" +
                                                   std::to_string(coord.y));
            parent->add_component<toy::scene::RuntimeObject>(
                "InstancePainter", "painted-instance colliders -- paint in Instance Paint mode");
            parent->add_component<TransformComponent>();
        }

        auto obj = std::make_unique<SceneObject>("instance");
        auto* tc = obj->add_component<TransformComponent>();
        tc->transform().set_position(r.position);
        tc->transform().set_rotation_quat(r.rotation);
        tc->transform().set_scale(r.scale);

        Collider* col = nullptr;
        switch (shape) {
            case InstanceCollision::Box: {
                auto* c = obj->add_component<BoxCollider>();
                c->set_size(size);
                col = c;
                break;
            }
            case InstanceCollision::Sphere: {
                auto* c = obj->add_component<SphereCollider>();
                c->set_radius(proto.collision_radius > 0.0f ? proto.collision_radius
                                                            : 0.5f * std::max(width, size.z));
                col = c;
                break;
            }
            case InstanceCollision::Capsule: {
                auto* c = obj->add_component<CapsuleCollider>();
                c->set_radius(proto.collision_radius > 0.0f ? proto.collision_radius : 0.5f * width);
                c->set_height(size.z);
                c->set_direction(2);   // Z, the world's up
                col = c;
                break;
            }
            case InstanceCollision::Mesh: {
                auto* c = obj->add_component<MeshCollider>();
                const std::string base = !proto.collision_mesh.empty()
                                             ? proto.collision_mesh
                                             : (proto.parts.empty() ? std::string()
                                                                    : proto.parts.front().proxy->mesh_path());
                if (assets_ != nullptr && !base.empty()) {
                    c->set_mesh(assets_->load<coopa::physx::geometry::TriangleMesh>("meshes/" + base + ".yaml"));
                } else {
                    log_once_("[InstancePainter] '" + proto.label() +
                              "' asks for collision: mesh, but the system has no AssetManager");
                }
                col = c;
                break;
            }
            case InstanceCollision::Auto:
            case InstanceCollision::None:
                break;
        }
        if (col != nullptr) {
            // The shape is fitted in the prototype's own space, so its offset rides with it.
            if (shape != InstanceCollision::Mesh) col->set_center(centre);
            if (!proto.physics_material.empty() && assets_ != nullptr) {
                col->set_material_asset(assets_->load<coopa::physx::dynamics::PhysicsMaterial>(
                    "physics_materials/" + proto.physics_material + ".yaml"));
            }
        }
        parent->add_child(std::move(obj));
    }
    if (parent == nullptr) return nullptr;
    SceneObject* raw = parent.get();
    painter.owner->add_child(std::move(parent));
    return raw;
}

void InstancePainterSystem::update_collision_(coopa::scene::Scene& scene, InstancePainter& painter, Painted& p,
                                              const glm::vec3& camera) {
    if (painter.collision_radius <= 0.0f || painter.owner == nullptr) {
        drop_collision_(painter, p);
        return;
    }
    bool any_collidable = false;
    for (const InstancePrototype& proto : painter.prototypes) {
        any_collidable = any_collidable || proto.collision != InstanceCollision::None;
    }
    if (!any_collidable) {
        drop_collision_(painter, p);
        return;
    }

    // The editor rebuilds an edited object, which destroys the runtime children hanging off it
    // along with it. The bookkeeping is dropped (never used to detach from the dead object) so
    // the chunks are simply built again under the new one.
    if (p.collision_owner != painter.owner) {
        if (!p.collision.empty()) physics_dirty_ = true;
        p.collision.clear();
        p.collision_revision.clear();
        p.collision_owner = painter.owner;
    }

    const float cs = painter.data->chunk_size();
    auto distance = [&](ChunkCoord c) {
        const glm::vec2 lo = painter.data->chunk_origin(c);
        const glm::vec2 d = glm::max(glm::max(lo - glm::vec2(camera), glm::vec2(camera) - (lo + glm::vec2(cs))),
                                     glm::vec2(0.0f));
        return glm::length(d);
    };
    const float keep = painter.collision_radius * 1.2f + cs;

    // Out of range, or the records under it changed: drop it (a changed one is rebuilt below).
    for (auto it = p.collision.begin(); it != p.collision.end();) {
        const ChunkCoord c(it->first.first, it->first.second);
        const bool stale = p.collision_revision[it->first] != painter.data->chunk_revision(c);
        if (distance(c) > keep || stale) {
            if (it->second != nullptr) painter.owner->detach_child(it->second);
            p.collision_revision.erase(it->first);
            it = p.collision.erase(it);
            physics_dirty_ = true;
        } else {
            ++it;
        }
    }

    for (ChunkCoord c : painter.data->chunks()) {
        const auto key = std::make_pair(c.x, c.y);
        if (p.collision.find(key) != p.collision.end()) continue;
        if (distance(c) > painter.collision_radius) continue;
        SceneObject* built = build_collision_chunk_(painter, c);
        // Recorded even when nothing was built: a chunk of grass has no colliders, and asking
        // again every frame would walk its records every frame.
        p.collision[key] = built;
        p.collision_revision[key] = painter.data->chunk_revision(c);
        if (built != nullptr) {
            scene.adopt(*built);
            physics_dirty_ = true;
        }
    }
}

void InstancePainterSystem::drop_collision_(InstancePainter& painter, Painted& p) {
    if (p.collision.empty()) return;
    // Only detach from the object the children were actually added to. A rebuilt painter has a
    // different owner, and the old one -- with its children -- is already gone.
    const bool same_owner = p.collision_owner == painter.owner && painter.owner != nullptr;
    for (auto& [key, obj] : p.collision) {
        if (obj != nullptr && same_owner) painter.owner->detach_child(obj);
    }
    p.collision.clear();
    p.collision_revision.clear();
    p.collision_owner = painter.owner;
    physics_dirty_ = true;
}

// --- render ---

void InstancePainterSystem::collect_render(coopa::scene::Scene& scene, const glm::vec3& camera,
                                           const std::function<bool(const glm::vec3&, const glm::vec3&)>& cull,
                                           render::ParticleFrameState& out) {
    // The scene is re-queried here rather than reusing anything remembered from execute():
    // see the declaration for why a painter pointer does not survive that gap in the editor.
    for (InstancePainter* painter : scene.get_components<InstancePainter>()) {
        if (painter == nullptr || !painter->enabled || !painter->data) continue;
        const auto it = painted_.find(painter->data->uid());
        if (it == painted_.end()) continue;   // rebuilt since the update: drawn again next frame
        const Painted& p = it->second;
        for (const auto& [coord, chunk] : p.cache.chunks()) {
            for (const InstanceRenderCache::PartBatch& b : chunk.parts) {
                if (b.matrices.empty() || b.prototype >= painter->prototypes.size()) continue;
                const InstancePrototype& proto = painter->prototypes[b.prototype];
                if (b.part >= proto.parts.size()) continue;
                auto* proxy = proto.parts[b.part].proxy;
                if (proxy == nullptr || !proxy->is_ready()) continue;
                proxy->cast_shadows = painter->cast_shadows && proto.cast_shadows;

                const glm::vec3 d = glm::max(glm::max(b.bounds_min - camera, camera - b.bounds_max), glm::vec3(0.0f));
                const std::uint32_t drawn = particles::TerrainScatterCache::thinned_count(
                    static_cast<std::uint32_t>(b.matrices.size()), glm::length(d), b.draw_distance, b.fade_start);
                if (drawn == 0) continue;
                if (cull && !cull(b.bounds_min, b.bounds_max)) continue;

                render::MeshInstanceBatch mb;
                mb.proxy = proxy;
                mb.matrices = b.matrices.data();
                // Null unless this prototype named a parent plant, which switches the shader's
                // parent-sway term off for every ordinary painted instance.
                mb.foliage = b.foliage.empty() ? nullptr : b.foliage.data();
                mb.foliage1 = b.foliage1.empty() ? nullptr : b.foliage1.data();
                mb.count = drawn;
                mb.bounds_min = b.bounds_min;
                mb.bounds_max = b.bounds_max;
                // Static: the renderer keeps the chunk in its own GPU buffer, uploaded once per
                // build, and (because the version does not move) treats it as not having moved.
                mb.static_key = batch_key(p.key, coord, b.prototype, b.part);
                mb.static_version = b.build_id;
                mb.static_total = static_cast<std::uint32_t>(b.matrices.size());
                out.meshes.push_back(mb);
            }
        }
    }
}

std::size_t InstancePainterSystem::instance_count() const {
    std::size_t n = 0;
    for (const auto& [uid, p] : painted_) n += p.cache.instance_count();
    return n;
}

// --- install ---

InstancePainterSystem* find_instance_painter_system(coopa::scene::Scene& scene) {
    return dynamic_cast<InstancePainterSystem*>(scene.find_system("InstancePainter"));
}

InstancePainterSystem* install_instance_painter_system(coopa::scene::Scene& scene,
                                                       coopa::asset::AssetManager* assets, int order) {
    if (auto* existing = find_instance_painter_system(scene)) return existing;
    auto sys = std::make_unique<InstancePainterSystem>(assets);
    InstancePainterSystem* raw = sys.get();
    scene.add_system(std::move(sys), order);
    return raw;
}

} // namespace instance
} // namespace toy
