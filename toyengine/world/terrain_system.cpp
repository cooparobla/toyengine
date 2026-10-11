#include <toyengine/world/terrain_system.h>

#include <cmath>

#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/allocator.h>
#include <physxcoopa/components/mesh_collider.h>
#include <physxcoopa/dynamics/physics_material.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

void TerrainSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    ++frame_;
    physics_dirty_ = false;
    for (TerrainComponent* terrain : scene.get_components<TerrainComponent>()) {
        tick_terrain_(*terrain, scene, ctx);
    }
    // One diff per frame however many chunk colliders came or went (PhysicsSystem::refresh()).
    if (physics_dirty_) {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) {
            physics->refresh();
        }
    }
}

void TerrainSystem::tick_terrain_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                   const coopa::scene::FrameContext& ctx) {
    terrain.begin_generation(ctx.jobs);
    terrain.poll_generation();
    terrain.poll_tile_sets();
    terrain.release_retired_collision(frame_, k_collision_keep_frames);

    // Retiring chunks are advanced even before the terrain is ready: a scene torn down
    // mid-generation still has handles to close.
    advance_retiring_(terrain);
    if (!terrain.is_ready() || ctx.jobs == nullptr) return;

    mark_edited_chunks_(terrain);
    const ChunkCoord centre = terrain.params.chunk_at(stream_centre_(terrain));
    want_chunks_(terrain, centre);
    release_distant_chunks_(terrain, centre, ctx);
    upload_finished_chunks_(terrain, scene);
    submit_queued_chunks_(terrain, centre, ctx);
}

glm::vec3 TerrainSystem::stream_centre_(const TerrainComponent& terrain) const {
    glm::mat4 to_local(1.0f);
    if (terrain.owner != nullptr) {
        if (auto* tc = terrain.owner->get_transform()) to_local = glm::inverse(tc->transform().get_world_matrix());
    }
    CameraComponent* camera = CameraComponent::main();
    if (camera == nullptr || camera->owner == nullptr || camera->owner->get_transform() == nullptr) {
        return glm::vec3(0.0f);   // the terrain's own origin
    }
    const glm::mat4 cam = camera->owner->get_transform()->transform().get_world_matrix();
    const glm::vec3 pos = glm::vec3(to_local * cam[3]);
    // The camera looks down its own -Z.
    const glm::vec3 fwd = glm::normalize(glm::vec3(to_local * glm::vec4(-glm::vec3(cam[2]), 0.0f)));

    // Stream around what the camera looks at, not where it stands: an orbit or editor camera
    // stands well off the ground (or off the map) looking at it. Only within reach -- a camera
    // looking at the horizon keeps streaming around itself.
    const float reach = 4.0f * static_cast<float>(terrain.params.view_radius + 1) * terrain.params.chunk_world_size();
    if (terrain.tiles && !terrain.tiles->empty()) {
        TileHit hit;
        if (terrain.tiles->raycast(pos, fwd, terrain.params.tile_size, terrain.params.height_step, reach, hit)) return hit.point;
    }
    if (fwd.z < -1e-3f) {
        const float t = -pos.z / fwd.z;   // the terrain's base plane
        if (t > 0.0f && t < reach) return pos + fwd * t;
    }
    return pos;
}

bool TerrainSystem::chunk_in_world(const TerrainComponent& terrain, const ChunkCoord& coord) {
    if (!terrain.tiles) return false;
    const std::int32_t size = terrain.params.chunk_size;
    return coord.x >= 0 && coord.y >= 0 && coord.x * size < terrain.tiles->width() &&
           coord.y * size < terrain.tiles->height();
}

void TerrainSystem::mark_edited_chunks_(TerrainComponent& terrain) {
    const TileMapData& tiles = *terrain.tiles;
    if (terrain.applied_tile_revision == tiles.revision()) return;
    const std::int32_t size = std::max(1, terrain.params.chunk_size);
    tiles.for_each_dirty_since(terrain.applied_tile_revision, [&](const IRect& rect) {
        // One tile wider: a chunk's skirt reads its neighbours' border columns.
        const std::int32_t cx0 = static_cast<std::int32_t>(std::floor((rect.x0 - 1) / static_cast<float>(size)));
        const std::int32_t cy0 = static_cast<std::int32_t>(std::floor((rect.y0 - 1) / static_cast<float>(size)));
        const std::int32_t cx1 = static_cast<std::int32_t>(std::floor(rect.x1 / static_cast<float>(size)));
        const std::int32_t cy1 = static_cast<std::int32_t>(std::floor(rect.y1 / static_cast<float>(size)));
        for (auto& [coord, chunk] : terrain.chunks()) {
            if (chunk.state == ChunkState::Retiring || chunk.state == ChunkState::Queued) continue;
            if (coord.x >= cx0 && coord.x <= cx1 && coord.y >= cy0 && coord.y <= cy1) chunk.stale = true;
        }
    });
    terrain.applied_tile_revision = tiles.revision();
}

void TerrainSystem::want_chunks_(TerrainComponent& terrain, const ChunkCoord& centre) {
    const std::int32_t radius = terrain.params.view_radius;
    for (std::int32_t dy = -radius; dy <= radius; ++dy) {
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            const ChunkCoord coord{centre.x + dx, centre.y + dy};
            if (!chunk_in_world(terrain, coord)) continue;
            if (terrain.chunks().find(coord) != terrain.chunks().end()) continue;

            TerrainChunk chunk;
            chunk.coord = coord;
            chunk.state = ChunkState::Queued;
            terrain.chunks().emplace(coord, std::move(chunk));
        }
    }
}

void TerrainSystem::release_distant_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                             const coopa::scene::FrameContext& ctx) {
    const std::int32_t keep = terrain.params.view_radius + 1;
    for (auto it = terrain.chunks().begin(); it != terrain.chunks().end();) {
        TerrainChunk& chunk = it->second;
        if (chunk.state == ChunkState::Retiring || chunk_distance(chunk.coord, centre) <= keep) {
            ++it;
            continue;
        }

        if (chunk.state == ChunkState::Meshing) {
            // Cancel only stops jobs that have not started; either way the handle has to
            // survive until the counter drains, so park it rather than erasing here.
            ctx.jobs->cancel(chunk.job);
            chunk.state = ChunkState::Retiring;
            chunk.build.reset(); // the job holds its own reference; this one is dead weight
            ++it;
            continue;
        }

        destroy_chunk_object_(terrain, chunk);
        it = terrain.chunks().erase(it);
    }
}

void TerrainSystem::advance_retiring_(TerrainComponent& terrain) {
    for (auto it = terrain.chunks().begin(); it != terrain.chunks().end();) {
        TerrainChunk& chunk = it->second;
        if (chunk.state != ChunkState::Retiring) {
            ++it;
            continue;
        }
        if (!chunk.job.is_complete()) {
            ++it;
            continue;
        }
        chunk.job.close();
        chunk.job = coopa::job::JobHandle{}; // same one-close rule as upload_finished_chunks_
        destroy_chunk_object_(terrain, chunk);
        it = terrain.chunks().erase(it);
    }
}

void TerrainSystem::upload_finished_chunks_(TerrainComponent& terrain, coopa::scene::Scene& scene) {
    int budget = std::max(1, terrain.max_chunk_jobs_per_frame);
    for (auto& entry : terrain.chunks()) {
        if (budget <= 0) break;
        TerrainChunk& chunk = entry.second;
        if (chunk.state != ChunkState::Meshing || !chunk.job.is_complete()) continue;

        chunk.job.close();
        // Cleared, not merely closed: a closed handle's pool slot is recycled, so leaving
        // the stale token in place would let ~TerrainComponent()'s drain close it a second
        // time -- which the CounterPool asserts on, by design.
        chunk.job = coopa::job::JobHandle{};
        chunk.state = ChunkState::Live;
        --budget;

        // An empty chunk is a real outcome, not a failure: a chunk of open ocean below the
        // waterline emits no faces at all. It stays Live with no object, so it is never
        // re-queued and never draws.
        // A rebuild replaces the chunk's object in this same frame, so it never shows a hole.
        destroy_chunk_object_(terrain, chunk);
        if (chunk.build && !chunk.build->mesh.empty()) {
            create_chunk_object_(terrain, scene, chunk);
        }
        chunk.build.reset();
    }
}

void TerrainSystem::submit_queued_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                           const coopa::scene::FrameContext& ctx) {
    const int budget = std::max(1, terrain.max_chunk_jobs_per_frame);

    pending_.clear();
    for (auto& entry : terrain.chunks()) {
        const TerrainChunk& c = entry.second;
        if (c.state == ChunkState::Queued || (c.state == ChunkState::Live && c.stale)) pending_.push_back(entry.first);
    }
    if (pending_.empty()) return;

    // Nearest first, so the ground under the camera appears before the horizon does. The tie
    // break on coordinates keeps the order deterministic for a given camera position, which
    // is what makes a scripted capture reproducible.
    std::sort(pending_.begin(), pending_.end(),
              [&centre](const ChunkCoord& a, const ChunkCoord& b) {
                  const std::int32_t da = chunk_distance(a, centre);
                  const std::int32_t db = chunk_distance(b, centre);
                  if (da != db) return da < db;
                  if (a.y != b.y) return a.y < b.y;
                  return a.x < b.x;
              });

    const int submitted_count = std::min<int>(budget, static_cast<int>(pending_.size()));
    for (int i = 0; i < submitted_count; ++i) {
        TerrainChunk& chunk = terrain.chunks().at(pending_[static_cast<std::size_t>(i)]);

        auto build = std::make_shared<ChunkBuildJob>();
        // Sampled here, on the owner thread: the job meshes its own copy, so the tiles may be
        // edited while it runs.
        sample_chunk_columns(*terrain.tiles, terrain.params, chunk.coord, build->pad);
        build->library = &terrain.library();
        build->params  = terrain.params;
        build->coord   = chunk.coord;
        build->collision = terrain.collision;

        chunk.build = build;
        chunk.job   = ctx.jobs->create_handle();
        chunk.state = ChunkState::Meshing;
        chunk.stale = false;

        // One job per chunk rather than one parallel_for over all of them: chunks finish and
        // appear individually, and a chunk that leaves view mid-flight can be cancelled on
        // its own without disturbing its neighbours.
        ctx.jobs->submit([build]() { build->run(); }, k_terrain_job_type, chunk.job, nullptr,
                         0u, coopa::job::Priority::Normal);
    }
}

void TerrainSystem::create_chunk_object_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                          TerrainChunk& chunk) {
    if (terrain.owner == nullptr) return;

    const ChunkMeshData& data = chunk.build->mesh;
    // buffer_count 1: a chunk mesh is written once and never rewritten, unlike a cloth's
    // per-frame ring (see Mesh::from_arrays()'s doc on why that is a count, not a bool).
    chunk.mesh = std::make_shared<Mesh>(
        Mesh::from_arrays(device_, allocator_, data.vertices, data.indices, 1));

    ++chunk.mesh_serial;
    const std::string id = chunk_asset_id_(terrain, chunk);
    auto handle = assets_.create<Mesh>(id, chunk.mesh);

    auto object = std::make_unique<coopa::scene::SceneObject>(chunk_object_name_(chunk.coord));
    auto* transform = object->add_component<TransformComponent>();
    transform->transform().set_position(chunk_origin(terrain.params, chunk.coord));

    auto* renderer = object->add_component<MeshRenderer>();
    renderer->material = terrain.material;
    // Chunks carry blend codes (encode_surface_blend()), not texture coordinates:
    // terrain_styled.frag picks each pixel's kind from them, takes its colour from the atlas and
    // adds world-space detail -- it needs the atlas grid and the cell size.
    renderer->material.shader        = "terrain_styled";
    renderer->material.shader_params = glm::vec4(static_cast<float>(k_atlas_columns),
                                                 static_cast<float>(k_atlas_rows),
                                                 terrain.params.height_step, terrain.params.tile_size);
    // Probe / GI bakes draw with stock shaders only and would misread those codes.
    renderer->affects_reflection_probes = false;
    renderer->set_mesh(std::move(handle));

    if (chunk.build->collision_mesh) {
        // Static: no Rigidbody, so the physics system makes it a fixed body. The mesh is the
        // drawn surface (built on the worker), so it matches whatever tile set drew it.
        auto* collider = object->add_component<coopa::physx::components::MeshCollider>();
        collider->set_mesh(chunk.build->collision_mesh);
        if (!terrain.physics_material.empty()) {
            collider->set_material_asset(assets_.load<coopa::physx::dynamics::PhysicsMaterial>(
                "physics_materials/" + terrain.physics_material + ".yaml"));
        }
        physics_dirty_ = true;
    }

    chunk.object = terrain.owner->add_child(std::move(object));
    // Stamps the Scene back-pointer onto the new components, exactly as
    // Scene::flush_commands() does for a deferred add -- see Scene::adopt().
    scene.adopt(*chunk.object);
}

void TerrainSystem::destroy_chunk_object_(TerrainComponent& terrain, TerrainChunk& chunk) {
    if (chunk.object != nullptr && terrain.owner != nullptr) {
        if (auto* collider = chunk.object->get_component<coopa::physx::components::MeshCollider>()) {
            terrain.retire_collision(collider->owned_mesh(), frame_);
            physics_dirty_ = true;
        }
        // The returned unique_ptr is destroyed immediately, which destroys the MeshRenderer
        // and with it the last AssetHandle on the chunk's mesh -- the refcount unload()
        // requires to have reached zero.
        terrain.owner->detach_child(chunk.object);
        chunk.object = nullptr;
    }
    if (chunk.mesh) {
        chunk.mesh.reset();
        assets_.unload(coopa::asset::AssetId::from_path(chunk_asset_id_(terrain, chunk)));
    }
}

std::string TerrainSystem::chunk_asset_id_(const TerrainComponent& terrain, const TerrainChunk& chunk) {
    const std::string owner = terrain.owner != nullptr ? terrain.owner->name() : "terrain";
    return "runtime/terrain/" + owner + "/" + std::to_string(chunk.coord.x) + "_" +
           std::to_string(chunk.coord.y) + "_" + std::to_string(chunk.mesh_serial);
}

TerrainSystem* install_terrain_system(coopa::scene::Scene& scene,
                                             coopa::gfx::core::Device& device,
                                             coopa::gfx::memory::Allocator& allocator,
                                             coopa::asset::AssetManager& assets,
                                             int order) {
    auto system = std::make_unique<TerrainSystem>(device, allocator, assets);
    TerrainSystem* raw = system.get();
    scene.add_system(std::move(system), order);
    return raw;
}

} // namespace world
} // namespace toy
