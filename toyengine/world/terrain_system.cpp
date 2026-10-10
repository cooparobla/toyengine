#include <toyengine/world/terrain_system.h>

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
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

void TerrainSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    for (TerrainComponent* terrain : scene.get_components<TerrainComponent>()) {
        tick_terrain_(*terrain, scene, ctx);
    }
}

void TerrainSystem::tick_terrain_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                   const coopa::scene::FrameContext& ctx) {
    terrain.begin_generation(ctx.jobs);
    terrain.poll_generation();
    terrain.poll_side_meshes();

    // Retiring chunks are advanced even before the terrain is ready: a scene torn down
    // mid-generation still has handles to close.
    advance_retiring_(terrain);
    if (!terrain.is_ready() || ctx.jobs == nullptr) return;

    const ChunkCoord centre = terrain.params.chunk_at(camera_position_(terrain));
    want_chunks_(terrain, centre);
    release_distant_chunks_(terrain, centre, ctx);
    upload_finished_chunks_(terrain, scene);
    submit_queued_chunks_(terrain, centre, ctx);
}

glm::vec3 TerrainSystem::camera_position_(const TerrainComponent& terrain) const {
    if (CameraComponent* camera = CameraComponent::main()) {
        if (camera->owner != nullptr) {
            if (auto* tc = camera->owner->get_transform()) {
                return glm::vec3(tc->transform().get_world_matrix()[3]);
            }
        }
    }
    if (terrain.owner != nullptr) {
        if (auto* tc = terrain.owner->get_transform()) {
            return glm::vec3(tc->transform().get_world_matrix()[3]);
        }
    }
    return glm::vec3(0.0f);
}

bool TerrainSystem::chunk_in_world(const TerrainComponent& terrain, const ChunkCoord& coord) {
    const std::int32_t tiles = terrain.sampler().tiles_per_axis();
    const std::int32_t size  = terrain.params.chunk_size;
    return coord.x >= 0 && coord.y >= 0 && coord.x * size < tiles && coord.y * size < tiles;
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
        if (entry.second.state == ChunkState::Queued) pending_.push_back(entry.first);
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
        build->sampler = &terrain.sampler();
        build->library = &terrain.library();
        build->params  = terrain.params;
        build->coord   = chunk.coord;

        chunk.build = build;
        chunk.job   = ctx.jobs->create_handle();
        chunk.state = ChunkState::Meshing;

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

    const std::string id = chunk_asset_id_(terrain, chunk.coord);
    auto handle = assets_.create<Mesh>(id, chunk.mesh);

    auto object = std::make_unique<coopa::scene::SceneObject>(chunk_object_name_(chunk.coord));
    auto* transform = object->add_component<TransformComponent>();
    transform->transform().set_position(chunk_origin(terrain.params, chunk.coord));

    auto* renderer = object->add_component<MeshRenderer>();
    renderer->material = terrain.material;
    if (terrain.library().has_styles()) {
        // Styled chunks carry blend codes (encode_surface_blend()); terrain_styled.frag picks
        // each pixel's kind from them, takes its colour from the atlas and adds world-space
        // detail -- it needs the atlas grid and the cell size.
        renderer->material.shader        = "terrain_styled";
        renderer->material.shader_params = glm::vec4(static_cast<float>(k_atlas_columns),
                                                     static_cast<float>(k_atlas_rows),
                                                     terrain.params.height_step, terrain.params.tile_size);
        // Its UVs are blend codes, not texture coordinates: probe / GI bakes, which draw with
        // stock shaders only, would misread them.
        renderer->affects_reflection_probes = false;
    } else if (terrain.params.greedy_merge) {
        // The greedy mesher writes tile-space UVs (TileMeshLibrary::encode_uv), which only
        // the `terrain` surface shader decodes; it reads the atlas grid from shader_params.
        renderer->material.shader        = "terrain";
        renderer->material.shader_params = glm::vec4(static_cast<float>(k_atlas_columns),
                                                     static_cast<float>(k_atlas_rows),
                                                     static_cast<float>(k_atlas_cell_texels), 0.0f);
        // Probe / GI bakes draw with stock shaders only and would misread those UVs.
        renderer->affects_reflection_probes = false;
    }
    renderer->set_mesh(std::move(handle));

    chunk.object = terrain.owner->add_child(std::move(object));
    // Stamps the Scene back-pointer onto the new components, exactly as
    // Scene::flush_commands() does for a deferred add -- see Scene::adopt().
    scene.adopt(*chunk.object);
}

void TerrainSystem::destroy_chunk_object_(TerrainComponent& terrain, TerrainChunk& chunk) {
    if (chunk.object != nullptr && terrain.owner != nullptr) {
        // The returned unique_ptr is destroyed immediately, which destroys the MeshRenderer
        // and with it the last AssetHandle on the chunk's mesh -- the refcount unload()
        // requires to have reached zero.
        terrain.owner->detach_child(chunk.object);
        chunk.object = nullptr;
    }
    if (chunk.mesh) {
        chunk.mesh.reset();
        assets_.unload(coopa::asset::AssetId::from_path(chunk_asset_id_(terrain, chunk.coord)));
    }
}

std::string TerrainSystem::chunk_asset_id_(const TerrainComponent& terrain, const ChunkCoord& coord) {
    const std::string owner = terrain.owner != nullptr ? terrain.owner->name() : "terrain";
    return "runtime/terrain/" + owner + "/" + std::to_string(coord.x) + "_" +
           std::to_string(coord.y);
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
