#include <toyengine/scene/destruction_system.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <coopa/asset/asset_handle.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/yaml/document.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/dynamics/physics_material.h>
#include <physxcoopa/system/physics_system.h>

#include <toyengine/scene/destructible.h>
#include <toyengine/scene/runtime_object.h>

namespace toy {
namespace scene {

namespace {

using coopa::gfx::engine::components::MeshRenderer;
using coopa::gfx::engine::data::Mesh;
using coopa::gfx::engine::data::SkinnedMeshSource;
using coopa::scene::TransformComponent;

/// Fracture jobs are their own type so the engine's diagnostics can tell them apart.
constexpr coopa::job::JobType k_fracture_job_type = 7;

/// Below this the chunk is a crumb: too small to read, not worth a body.
constexpr float k_min_chunk_volume_fraction = 1e-4f;

} // namespace

DestructionSystem::~DestructionSystem() {
    // Same contract as TerrainComponent's destructor: a fracture job holds a shared_ptr to its
    // own output, but the handles it was given must be closed or the JobEngine reports leaked
    // slots at shutdown.
    if (jobs_ == nullptr) return;
    for (auto& [key, fracture] : cache_) {
        if (!fracture.job.is_valid()) continue;
        jobs_->wait_for(fracture.job);
        fracture.job.close();
    }
}

std::string DestructionSystem::cache_key_(const std::string& mesh_path, const Destructible& d) {
    return mesh_path + "|" + std::to_string(d.pieces) + "|" + std::to_string(d.seed) + "|" +
           std::to_string(static_cast<int>(d.irregularity * 100.0f)) + "|" +
           std::to_string(static_cast<int>(d.collider_fill * 100.0f)) + "|" +
           std::to_string(d.max_boxes);
}

bool DestructionSystem::ensure_fracture_(Destructible& d, const std::string& mesh_path, Fracture*& out) {
    const std::string key = cache_key_(mesh_path, d);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        Fracture& f = it->second;
        if (!f.ready && f.job.is_valid() && f.job.is_complete()) {
            f.job.close();
            f.job = coopa::job::JobHandle{};
            f.ready = true;
        }
        out = &f;
        return f.ready;
    }

    // The geometry is read from the mesh FILE rather than through the asset manager: the same
    // file is already loaded as a GPU Mesh for the renderer, and one file cannot be two asset
    // types at once ("refusing to reinterpret its payload"). SkinnedMeshSource::from_node() is
    // the CPU-side parse of exactly that format, so this is the same vertices the renderer
    // draws, read once and cached in the fracture.
    const std::string file = assets_.source().resolve("meshes/" + mesh_path + ".yaml");
    SkinnedMeshSource src;
    try {
        src = SkinnedMeshSource::from_node(coopa::yaml::load_document(file));
    } catch (const std::exception& e) {
        // Cached as an empty, ready fracture: a mesh that cannot be read must not be re-read
        // every frame for the life of the scene.
        Fracture dead;
        dead.pieces = std::make_shared<std::vector<destruct::FracturePiece>>();
        dead.ready = true;
        std::fprintf(stderr, "[destruction] cannot read '%s' to fracture it: %s\n", file.c_str(), e.what());
        out = &cache_.emplace(key, std::move(dead)).first->second;
        return true;
    }

    Fracture fracture;
    fracture.pieces = std::make_shared<std::vector<destruct::FracturePiece>>();
    destruct::FractureSettings settings;
    settings.pieces = std::max(1, d.pieces);
    settings.seed = d.seed;
    settings.irregularity = d.irregularity;
    settings.collider_fill = d.collider_fill;
    settings.max_boxes = std::max(1, d.max_boxes);

    auto pieces = fracture.pieces;
    // Copied, not borrowed: the job outlives this call and the asset slot may be reloaded.
    auto vertices = std::make_shared<std::vector<destruct::Vertex>>(src.vertices);
    auto indices = std::make_shared<std::vector<std::uint32_t>>(src.indices);
    if (jobs_ != nullptr) {
        fracture.job = jobs_->create_handle();
        jobs_->submit([pieces, vertices, indices, settings]() {
            *pieces = destruct::fracture_mesh(*vertices, *indices, settings);
        }, k_fracture_job_type, fracture.job, nullptr, 0u, coopa::job::Priority::Low);
    } else {
        *pieces = destruct::fracture_mesh(*vertices, *indices, settings);
        fracture.ready = true;
    }
    out = &cache_.emplace(key, std::move(fracture)).first->second;
    return out->ready;
}

int DestructionSystem::spawn_chunks_(coopa::scene::Scene& scene, Destructible& d, const Fracture& fracture) {
    coopa::scene::SceneObject* owner = d.owner;
    if (owner == nullptr || fracture.pieces == nullptr) return 0;
    auto* owner_tc = owner->get_transform();
    if (owner_tc == nullptr) return 0;

    const glm::mat4 world = owner_tc->transform().get_world_matrix();
    const glm::mat3 basis(world);
    const auto* renderer = owner->get_component<MeshRenderer>();

    const std::vector<destruct::FracturePiece>& pieces = *fracture.pieces;
    float largest = 0.0f;
    for (const destruct::FracturePiece& p : pieces) largest = std::max(largest, p.volume);

    const int wanted = d.max_chunks > 0 ? std::min<int>(d.max_chunks, static_cast<int>(pieces.size()))
                                        : static_cast<int>(pieces.size());
    int spawned = 0;
    for (int i = 0; i < wanted; ++i) {
        const destruct::FracturePiece& piece = pieces[static_cast<std::size_t>(i)];
        if (piece.volume < largest * k_min_chunk_volume_fraction) continue;

        auto object = std::make_unique<coopa::scene::SceneObject>(
            "chunk_" + std::to_string(spawned_) + "_" + std::to_string(i));
        object->add_component<RuntimeObject>("Destruction", "a chunk of " + owner->name());

        // The piece's own space, placed where that space sits in the world: its box centre
        // under the owner's transform, turned by the owner's rotation AND by the piece's own
        // principal frame (fracture.h) -- the frame its collider box is measured in.
        auto* tc = object->add_component<TransformComponent>();
        tc->transform().set_position(glm::vec3(world * glm::vec4(piece.centre, 1.0f)));
        const glm::quat owner_rotation = glm::quat_cast(glm::mat3(
            glm::normalize(basis[0]), glm::normalize(basis[1]), glm::normalize(basis[2])));
        tc->transform().set_rotation_quat(owner_rotation * piece.orientation);
        const glm::vec3 scale(glm::length(basis[0]), glm::length(basis[1]), glm::length(basis[2]));
        tc->transform().set_scale(scale);

        auto* mesh_renderer = object->add_component<MeshRenderer>();
        if (renderer != nullptr) mesh_renderer->material = renderer->material;
        auto gpu = std::make_shared<Mesh>(Mesh::from_arrays(device_, allocator_, piece.vertices, piece.indices, 1));
        mesh_renderer->set_mesh(assets_.create<Mesh>(
            "chunk:" + std::to_string(spawned_) + ":" + std::to_string(i), gpu));

        // Boxes in the piece's own frame: physxcoopa has no convex hull and a triangle mesh
        // cannot be a dynamic shape (see the header), so a shard collides as the one to four
        // boxes fracture.h fitted to it. One box goes on the chunk itself; several become
        // child colliders, which PhysicsSystem gathers into a single compound body under the
        // Rigidbody below -- one body, several shapes, no joints.
        coopa::asset::AssetHandle<coopa::physx::dynamics::PhysicsMaterial> material;
        if (!d.chunk_physics_material.empty()) {
            material = assets_.load<coopa::physx::dynamics::PhysicsMaterial>(
                "physics_materials/" + d.chunk_physics_material + ".yaml");
        }
        const std::vector<destruct::FractureBox>& boxes = piece.boxes;
        if (boxes.size() <= 1) {
            const glm::vec3 half = boxes.empty() ? piece.half_extent : boxes.front().half_extent;
            const glm::vec3 centre = boxes.empty() ? glm::vec3(0.0f) : boxes.front().centre;
            auto* collider = object->add_component<coopa::physx::components::BoxCollider>();
            collider->set_size(glm::max(half * 2.0f, glm::vec3(1e-3f)));
            collider->set_center(centre);
            if (material.is_valid()) collider->set_material_asset(material);
        } else {
            for (std::size_t b = 0; b < boxes.size(); ++b) {
                auto part = std::make_unique<coopa::scene::SceneObject>("box_" + std::to_string(b));
                auto* part_tc = part->add_component<TransformComponent>();
                part_tc->transform().set_position(boxes[b].centre);
                // add_child() links ownership only -- a child's Transform must be parented by
                // hand (see SceneObject::add_child). Without this the box's world matrix IS its
                // local one, and PhysicsSystem, which places a compound's shapes from
                // world_child - world_root, would hang every shape a whole chunk position away
                // from the body it belongs to: the chunk then falls through the floor its
                // collision is standing on, somewhere near the origin.
                part_tc->set_parent_transform(&tc->transform());
                auto* collider = part->add_component<coopa::physx::components::BoxCollider>();
                collider->set_size(glm::max(boxes[b].half_extent * 2.0f, glm::vec3(1e-3f)));
                if (material.is_valid()) collider->set_material_asset(material);
                object->add_child(std::move(part));
            }
        }

        auto* body = object->add_component<coopa::physx::components::RigidbodyComponent>();
        const float volume = piece.volume * scale.x * scale.y * scale.z;
        body->mass = std::max(0.02f, volume * std::max(1.0f, d.chunk_density));

        coopa::scene::SceneObject* raw = scene.add_root_object(std::move(object));
        scene.adopt(*raw);

        Chunk chunk;
        chunk.object = raw;
        chunk.lifetime = d.chunk_lifetime;
        chunk.fade = d.chunk_fade;
        chunks_.push_back(chunk);
        ++spawned;
    }
    ++spawned_;
    if (std::getenv("DESTRUCT_DEBUG") != nullptr) {
        std::fprintf(stderr, "[destruct] %s broke into %d chunks\n", owner->name().c_str(), spawned);
    }

    // Gone, not deleted: a name, a script or a selection still resolves to the object, and the
    // chunks are what is drawn from here on.
    owner->set_active(false);
    physics_dirty_ = true;

    // The push: away from where it was hit, plus a share of the blow itself. Applied after the
    // bodies exist, so a body's velocity is set rather than queued.
    const glm::vec3 centre = glm::vec3(world[3]);
    const glm::vec3 point = glm::length(d.hit_point()) > 0.0f ? d.hit_point() : centre;
    for (std::size_t i = chunks_.size() - static_cast<std::size_t>(spawned); i < chunks_.size(); ++i) {
        auto* body = chunks_[i].object->get_component<coopa::physx::components::RigidbodyComponent>();
        auto* tc = chunks_[i].object->get_transform();
        if (body == nullptr || tc == nullptr) continue;
        glm::vec3 away = tc->transform().position() - point;
        const float len = glm::length(away);
        away = len > 1e-4f ? away / len : glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 velocity = away * d.scatter + d.hit_impulse() * d.impact_transfer;
        body->add_impulse(velocity * body->mass);
    }
    return spawned;
}

void DestructionSystem::age_chunks_(coopa::scene::Scene& scene, float dt) {
    for (std::size_t i = 0; i < chunks_.size();) {
        Chunk& chunk = chunks_[i];
        if (chunk.object == nullptr || chunk.lifetime <= 0.0f) { ++i; continue; }
        chunk.age += dt;
        const float over = chunk.age - chunk.lifetime;
        if (over < 0.0f) { ++i; continue; }
        if (chunk.fade > 0.0f && over < chunk.fade) {
            // Sinking out of sight beats vanishing: debris that pops out mid-view reads as a
            // bug, and there is no per-object fade in the opaque pass to dissolve it with.
            if (auto* tc = chunk.object->get_transform()) {
                const glm::vec3 p = tc->transform().position();
                tc->transform().set_position(p - glm::vec3(0.0f, 0.0f, dt * 2.0f * glm::length(chunk.object->get_transform()->transform().scale())));
            }
            ++i;
            continue;
        }
        scene.remove_root_object(chunk.object);
        chunks_[i] = chunks_.back();
        chunks_.pop_back();
        physics_dirty_ = true;
    }
}

void DestructionSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    std::vector<Destructible*> breakables = scene.get_components<Destructible>();
    physics_dirty_ = false;

    for (Destructible* d : breakables) {
        if (d == nullptr || d->owner == nullptr || d->shattered()) continue;
        const auto* renderer = d->owner->get_component<MeshRenderer>();
        if (renderer == nullptr || renderer->mesh_path().empty()) continue;

        Fracture* fracture = nullptr;
        const bool ready = (d->prefracture || d->pending()) &&
                           ensure_fracture_(*d, renderer->mesh_path(), fracture);
        if (!d->pending() || !ready || fracture == nullptr) continue;
        d->mark_shattered(spawn_chunks_(scene, *d, *fracture));
    }

    age_chunks_(scene, ctx.delta_time);

    if (physics_dirty_) {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) {
            physics->refresh();
        }
    }
}

DestructionSystem* install_destruction_system(coopa::scene::Scene& scene,
                                              coopa::gfx::core::Device& device,
                                              coopa::gfx::memory::Allocator& allocator,
                                              coopa::asset::AssetManager& assets,
                                              coopa::job::JobEngine* jobs) {
    if (auto* existing = scene.find_system("Destruction")) {
        return dynamic_cast<DestructionSystem*>(existing);
    }
    auto system = std::make_unique<DestructionSystem>(device, allocator, assets, jobs);
    DestructionSystem* raw = system.get();
    scene.add_system(std::move(system), 70);
    return raw;
}

} // namespace scene
} // namespace toy
