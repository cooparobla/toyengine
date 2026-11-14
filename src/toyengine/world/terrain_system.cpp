#include <toyengine/world/terrain_system.h>

#include <algorithm>

#include <toyengine/scene/runtime_object.h>

#include <toyengine/water/water_body.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_id.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/debug/logger.h>
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
#include <toyengine/world/terrain_detail_texture.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/world/terra_store.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

namespace {

/**
 * @brief The priority a chunk's meshing job is submitted at, by its distance from the centre.
 *
 * The queue is already sorted nearest-first, but submission order only decides what is handed
 * over -- not what a worker picks up next. Normal for the camera's own chunk and the ring around
 * it (the ground a player is standing on, and about to walk onto), Low for everything beyond.
 *
 * Nothing here runs above Normal, and the bulk runs at Low, because a chunk mesh is BACKGROUND
 * work in the job engine's sense (job/README.md, "Keeping background work out of frame-critical
 * waits"): it is long and unsliced, so any frame thread that picks one up mid-wait bills a whole
 * chunk -- mesh, weld, three LOD levels, a collision BVH and the water -- to whatever system
 * happened to be waiting. The frame's own loops wait with a Normal help floor, so Low chunks are
 * invisible to them; the near ring stays at Normal to keep beating the far ring to a worker.
 * Near-before-far does not depend on this: submit_queued_chunks_() sorts the queue by distance.
 */
coopa::job::Priority chunk_priority(std::int32_t distance, std::int32_t view_radius) {
    (void)view_radius;
    if (distance <= 1) return coopa::job::Priority::Normal;
    return coopa::job::Priority::Low;
}

} // namespace

/**
 * @brief See terrain_system.h. Taking LOD 0's index count alone would under-report a coarse-built
 *        chunk by most of what it is actually holding, which is the quiet way to get this wrong.
 */
std::uint64_t mesh_bytes(std::uint32_t vertex_count, std::uint32_t lod0_index_count,
                         const std::vector<coopa::gfx::engine::data::MeshLod>& lods) {
    static_assert(sizeof(coopa::gfx::engine::data::Vertex) == 48,
                  "mesh_bytes() reports the vertex buffer from sizeof(Vertex); keep the two in step.");
    std::uint64_t indices = lod0_index_count;
    for (const auto& lod : lods) {
        indices = std::max<std::uint64_t>(indices, static_cast<std::uint64_t>(lod.first_index) +
                                                       static_cast<std::uint64_t>(lod.index_count));
    }
    return sizeof(coopa::gfx::engine::data::Vertex) * static_cast<std::uint64_t>(vertex_count) +
           sizeof(std::uint32_t) * indices;
}

std::uint64_t mesh_bytes(const coopa::gfx::engine::data::Mesh& mesh) {
    return mesh_bytes(mesh.vertex_count(), mesh.index_count(), mesh.lods());
}


void TerrainSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    ++frame_;
    physics_dirty_ = false;
    wanted_chunks_ = 0;
    unfinished_chunks_ = 0;
    terrain_baking_ = false;
    resident_mesh_bytes_ = 0;
    resident_chunks_ = 0;
    drawn_chunks_ = 0;
    coarse_chunks_ = 0;
    relief_peak_ = 0.0f;
    // Baked on the first frame that has terrain, not in the constructor: the bake submits a
    // one-shot upload, and a system is constructed before the device is necessarily ready to
    // take one. It registers itself with the asset manager, so this is one upload per run.
    const auto all_terrains = scene.get_components<TerrainComponent>();
    const bool any_tiles = std::any_of(all_terrains.begin(), all_terrains.end(), [](const TerrainComponent* t) {
        return t->mode == TerrainMode::Tiles;
    });
    if (!detail_texture_.is_loaded() && any_tiles) {
        coopa::gfx::command::CommandPool pool(device_, device_.graphics_family(), true);
        detail_texture_ = bake_terrain_detail_texture(device_, allocator_, pool, assets_, 512,
                                                      16.0f, ctx.jobs);
    }
    // The lookup returns every terrain in the scene (one component type, two modes); the
    // heightmap ones are HeightmapTerrainSystem's, at order 61.
    std::unordered_set<TerrainComponent*> live;
    for (TerrainComponent* terrain : scene.get_components<TerrainComponent>()) {
        if (terrain->mode != TerrainMode::Tiles) continue;
        // get_components() already skips a terrain whose own object is inactive; this also
        // catches an inactive ANCESTOR (an EnvPresetSwitcher preset group, say), which it does
        // not -- see active_in_hierarchy()'s doc.
        if (terrain->owner == nullptr || !active_in_hierarchy(*terrain->owner)) continue;
        // Not ready yet means the world bake or the tile-set bake is still running, and no chunk
        // has been asked for -- a zero wanted count that must not read as "nothing left to do".
        if (!terrain->is_ready() || terrain->baking_) terrain_baking_ = true;
        tick_terrain_(*terrain, scene, ctx);
        measure_(*terrain);
        live.insert(terrain);
    }
    // Timed here rather than per terrain: a scene may hold several, and what a loading screen and
    // a cost comparison both want is when the LAST of them finished. Frozen once warm, so a
    // later edit's re-mesh does not overwrite the figure the load is judged by.
    if (!load_timing_ && load_ms_ == 0.0 && wanted_chunks_ > 0) {
        load_timing_ = true;
        load_start_ = std::chrono::steady_clock::now();
    }
    if (load_timing_ && !terrain_baking_ && unfinished_chunks_ == 0) {
        load_timing_ = false;
        load_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start_).count();
        // Logged as well as watched: the watch lines need the overlay up, and the figures a
        // face_tiles or lods: change is judged against are wanted from a headless run too.
        coopa::debug::Logger("terrain").info(
            "loaded " + std::to_string(resident_chunks_) + " chunks, " +
            std::to_string(resident_mesh_bytes_ / (1024 * 1024)) + " MB of mesh, in " +
            std::to_string(static_cast<int>(load_ms_)) + " ms" +
            (relief_peak_ > 0.0f ? " (relief peak " + std::to_string(relief_peak_) + ")" : ""));
        // TOY_TERRAIN_SAVE: writes a freshly baked world to its `.terra` file once the load is
        // done. Normally saving is the editor's business (its Terrain and Tile panels call
        // TerraStore::save_dirty()), which leaves no way to regenerate a world from a headless
        // run -- and regenerating is exactly what changing `face_tiles`, `chunk_size` or a
        // generator setting forces, because none of those invalidates the stored file.
        // Deliberately env-gated and off by default: a game run must never rewrite its assets.
        if (const char* save = std::getenv("TOY_TERRAIN_SAVE");
            save != nullptr && std::strcmp(save, "0") != 0) {
            if (TerraStore::instance().any_dirty()) {
                const int written = TerraStore::instance().save_dirty();
                coopa::debug::Logger("terrain").info("TOY_TERRAIN_SAVE: wrote " +
                                                     std::to_string(written) + " .terra file(s)");
            }
        }
    }
    // Anything streamed last frame but not this one went inactive (or an ancestor did): nothing
    // else will ever ask this terrain's chunks to go away, so release them now instead of
    // leaking every chunk object, GPU mesh and collider it built for as long as the scene runs.
    for (TerrainComponent* terrain : streamed_) {
        if (live.find(terrain) == live.end()) release_terrain_(*terrain, ctx);
    }
    streamed_ = std::move(live);
    // A terrain release_terrain_() just parked mid-mesh chunks for is kept here even though it
    // no longer appears in streamed_ at all -- tick_terrain_() would otherwise be the only path
    // that drains a Retiring chunk, and an inactive terrain never reaches it again.
    for (auto it = releasing_.begin(); it != releasing_.end();) {
        TerrainComponent* terrain = *it;
        advance_retiring_(*terrain);
        if (terrain->chunks().empty()) it = releasing_.erase(it);
        else ++it;
    }
    after_terrains_(scene);
    // One diff per frame however many chunk colliders came or went (PhysicsSystem::refresh()).
    if (physics_dirty_) {
        if (auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) {
            physics->refresh();
        }
    }
}

void TerrainSystem::measure_(const TerrainComponent& terrain) {
    // One pass over the chunk map, after it has been ticked. A handful of loads per chunk, so a
    // 1536-chunk world is tens of microseconds -- cheaper than gating it would be, and gating
    // would need the debug overlay's state inside world/, which nothing here depends on.
    for (const auto& [coord, chunk] : terrain.chunks()) {
        (void)coord;
        if (!chunk.mesh) continue;
        ++resident_chunks_;
        resident_mesh_bytes_ += mesh_bytes(*chunk.mesh);
        if (chunk.object != nullptr && chunk.object->active()) ++drawn_chunks_;
        if (chunk.coarse) ++coarse_chunks_;
    }
    relief_peak_ = std::max(relief_peak_, relief_of_(terrain));
}

void TerrainSystem::tick_terrain_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                   const coopa::scene::FrameContext& ctx) {
    terrain.set_jobs(ctx.jobs);
    terrain.set_streaming_system(this);
    terrain.poll_tile_sets();
    // After the map generator system (order 59) has had its turn, because an apply landing this
    // frame resizes the grid the field is a view over.
    terrain.sync_field();
    terrain.release_retired_collision(frame_, k_collision_keep_frames);

    // Retiring chunks are advanced even before the terrain is ready: a scene torn down
    // mid-generation still has handles to close.
    advance_retiring_(terrain);
    if (!terrain.is_ready() || ctx.jobs == nullptr) return;

    advance_water_settles_(terrain);
    mark_edited_chunks_(terrain);
    stream_(terrain, scene, ctx);
}

void TerrainSystem::stream_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                            const coopa::scene::FrameContext& ctx) {
    // The flat policy: a square of chunks around the camera, and whatever leaves it is released.
    const ChunkCoord centre = latched_centre_(terrain);
    want_chunks_(terrain, centre, scene);
    release_distant_chunks_(terrain, centre, ctx);
    evict_dormant_chunks_(terrain);
    upload_finished_chunks_(terrain, scene);
    submit_queued_chunks_(terrain, centre, ctx);
}

void TerrainSystem::order_chunks_(const TerrainComponent& terrain,
                                  std::vector<ChunkCoord>& coords) const {
    // Nearest the streaming centre first, so the ground under the camera is built and uploaded
    // before the horizon's. The coordinate tie-break keeps the order deterministic for a given
    // camera position, which is what makes a scripted capture reproducible.
    const ChunkCoord centre = terrain.stream_centre.centre;
    std::sort(coords.begin(), coords.end(), [&](const ChunkCoord& a, const ChunkCoord& b) {
        const std::int32_t da = chunk_distance(a, centre);
        const std::int32_t db = chunk_distance(b, centre);
        if (da != db) return da < db;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
}

coopa::job::Priority TerrainSystem::chunk_job_priority_(const TerrainComponent& terrain,
                                                        const ChunkCoord& coord) const {
    return chunk_priority(chunk_distance(coord, terrain.stream_centre.centre),
                          terrain.params.view_radius);
}

float TerrainSystem::relief_of_(const TerrainComponent&) const {
    return 0.0f;   // a flat terrain measures no relief; nothing it feeds has any use for one
}

void TerrainSystem::after_terrains_(coopa::scene::Scene&) {}

ChunkCoord TerrainSystem::latched_centre_(TerrainComponent& terrain) const {
    const ChunkCoord candidate = terrain.params.chunk_at(stream_centre_(terrain));
    return latch_stream_centre(terrain.stream_centre, candidate, terrain.params.view_radius);
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

    // Where the camera STANDS, whenever that is over the terrain -- however high above it. It is
    // the only candidate a turn of the head cannot move, and for a camera over the map it is
    // also the right answer: what it can see is centred on itself, not on whatever its ray
    // happens to touch. One chunk of slack at the edges, so standing just off the coast still
    // loads the coast.
    const float reach = 4.0f * static_cast<float>(terrain.params.view_radius + 1) * terrain.params.chunk_world_size();
    if (!terrain.field.empty()) {
        const float slack = terrain.params.chunk_world_size();
        const float w = static_cast<float>(terrain.field.geometry().tiles_x) * terrain.params.tile_size;
        const float h = static_cast<float>(terrain.field.geometry().tiles_y) * terrain.params.tile_size;
        if (pos.x > -slack && pos.y > -slack && pos.x < w + slack && pos.y < h + slack) return pos;

        // Off the map: stream around what it looks at instead, so an orbit or editor camera
        // parked outside the world still loads the part of it that it is framing.
        TileHit hit;
        if (terrain.field.data()->raycast_columns(pos, fwd, reach, hit)) return hit.point;
    }
    if (fwd.z < -1e-3f) {
        const float t = -pos.z / fwd.z;   // the terrain's base plane
        if (t > 0.0f && t < reach) return pos + fwd * t;
    }
    return pos;
}

bool TerrainSystem::chunk_in_world(const TerrainComponent& terrain, const ChunkCoord& coord) {
    if (terrain.field.empty()) return false;
    const std::int32_t size = terrain.params.chunk_size;
    return coord.x >= 0 && coord.y >= 0 && coord.x * size < terrain.field.geometry().tiles_x &&
           coord.y * size < terrain.field.geometry().tiles_y;
}

void TerrainSystem::mark_edited_chunks_(TerrainComponent& terrain) {
    TerraData& data = *terrain.field.data();
    if (terrain.applied_tile_revision == data.revision()) return;
    const std::int32_t size = std::max(1, terrain.params.chunk_size);
    const std::int32_t width = data.geometry().tiles_x, height = data.geometry().tiles_y;
    // Only the tile plane ever changes in Tiles mode -- the height/layer cursors start at their
    // current value, so for_each_dirty_since() reports nothing new from planes nothing writes.
    const TerraCursor since{terrain.applied_tile_revision, data.cursor().heights, data.cursor().layers};
    data.for_each_dirty_since(since, [&](const IRect& rect) {
        if (terrain.topology != nullptr) {
            // Everything dirty at once: every chunk is stale, so say that directly. The walk
            // below is per TILE, and a large terrain has a great many -- 6144 x 1024 on
            // a 1536-chunk world, which with the nine lookups each is 57 million hash probes on the
            // owner thread in one frame, for an answer that is just "all of them". It is not a
            // rare path either: TerraData reports the whole grid after any mark_all_dirty() (a
            // load, a bake, a reshape) and whenever its dirty journal overflows.
            if (rect.x0 <= 0 && rect.y0 <= 0 && rect.x1 >= width && rect.y1 >= height) {
                for (auto& [coord, chunk] : terrain.chunks()) {
                    (void)coord;
                    if (chunk.state == ChunkState::Retiring || chunk.state == ChunkState::Queued) {
                        continue;
                    }
                    chunk.stale = true;
                }
                return;
            }
            // A tile on a region's edge is in the next region's skirt too: mark the chunks of
            // every tile the rect touches and of each of their neighbours, stepped through the
            // layout so the step crosses a boundary the way the layout joins it.
            const auto mark = [&](const glm::ivec2& t) {
                const ChunkCoord c{t.x / size, t.y / size};
                auto it = terrain.chunks().find(c);
                if (it != terrain.chunks().end() && it->second.state != ChunkState::Retiring &&
                    it->second.state != ChunkState::Queued) it->second.stale = true;
            };
            for (int y = std::max(0, rect.y0); y < std::min(height, rect.y1); ++y)
                for (int x = std::max(0, rect.x0); x < std::min(width, rect.x1); ++x)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) mark(terrain.topology->neighbour(x, y, dx, dy));
            return;
        }
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
    terrain.applied_tile_revision = data.revision();
}


void TerrainSystem::advance_water_settles_(TerrainComponent& terrain) {
    if (terrain.water_settles.empty() || terrain.field.empty()) return;
    // One budget shared over however many settles are in flight, oldest first, so digging again
    // while the last flood is still running cannot multiply the per-frame cost.
    std::size_t budget = static_cast<std::size_t>(std::max(1, terrain.water_settle_budget));
    while (!terrain.water_settles.empty() && budget > 0) {
        WaterSettle& settle = terrain.water_settles.front();
        const std::size_t before = settle.spent;
        step_settle(terrain.field, settle, budget);
        budget -= std::min(budget, settle.spent - before);
        // Whatever it filled is journalled, so mark_edited_chunks_() -- running straight after
        // this -- stales exactly the chunks the water reached and they re-mesh with it.
        if (settle.done()) terrain.water_settles.pop_front();
    }
}






void TerrainSystem::want_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                                 coopa::scene::Scene& scene) {
    const std::int32_t radius = terrain.params.view_radius;
    // Counted here rather than in a second walk: this scan already visits exactly the wanted set
    // once a frame, and warm_up_state() needs nothing more than its size and how much of it is
    // not Live yet. Accumulated, not assigned -- execute() zeroes the pair and a scene may hold
    // more than one terrain.
    for (std::int32_t dy = -radius; dy <= radius; ++dy) {
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            const ChunkCoord coord{centre.x + dx, centre.y + dy};
            if (!chunk_in_world(terrain, coord)) continue;
            ++wanted_chunks_;

            auto it = terrain.chunks().find(coord);
            if (it != terrain.chunks().end()) {
                TerrainChunk& chunk = it->second;
                chunk.last_used_frame = frame_;   // what the Dormant set is evicted by
                if (chunk.state == ChunkState::Dormant) revive_chunk_(terrain, scene, chunk);
                if (chunk.state == ChunkState::Live) {
                    sync_chunk_collision_(terrain, chunk, terrain_wants_collision(terrain, coord, centre));
                } else {
                    ++unfinished_chunks_;
                }
                if (chunk.state == ChunkState::Live && (chunk.stale || chunk.wants_collision)) {
                    ++unfinished_chunks_;
                }
                continue;
            }
            ++unfinished_chunks_;

            TerrainChunk chunk;
            chunk.coord           = coord;
            chunk.state           = ChunkState::Queued;
            chunk.last_used_frame = frame_;
            terrain.chunks().emplace(coord, std::move(chunk));
        }
    }
}

void TerrainSystem::revive_chunk_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                                  TerrainChunk& chunk) {
    chunk.state = ChunkState::Live;
    // An empty chunk (open sea below the waterline draws nothing) parks as a state and no
    // object: reviving it is just the state, and it is never re-meshed for nothing.
    if (!chunk.parked || terrain.owner == nullptr) return;
    const bool collides = chunk.parked->get_component<coopa::physx::components::MeshCollider>() != nullptr;
    chunk.object = terrain.owner->add_child(std::move(chunk.parked));
    // Stamps the Scene back-pointer onto the components again -- see Scene::adopt().
    scene.adopt(*chunk.object);
    if (collides) physics_dirty_ = true;
}

void TerrainSystem::sync_chunk_collision_(TerrainComponent& terrain, TerrainChunk& chunk, bool wanted) {
    // Raised the moment the chunk is inside the radius without a collider, and dropped again the
    // moment it is not -- which is what a camera drifting along the boundary does every few
    // frames, and it must not leave a job's worth of work behind each time. Cleared at the
    // submit, where `has_collision` takes over as the record.
    chunk.wants_collision = wanted && !chunk.has_collision;
    if (wanted == chunk.has_collision) return;
    if (!wanted) {
        // Letting go needs no geometry: drop the component and retire the shape. The chunk keeps
        // drawing the mesh it already has.
        if (chunk.object != nullptr) {
            if (auto* collider = chunk.object->get_component<coopa::physx::components::MeshCollider>()) {
                terrain.retire_collision(collider->owned_mesh(), frame_);
                chunk.object->remove_component(collider);
                physics_dirty_ = true;
            }
        }
        chunk.has_collision = false;
        return;
    }
    // Coming inward needs a collision mesh this chunk was never built with. `wants_collision`
    // above is the whole request: nothing the chunk draws changes, so nothing it draws is
    // rebuilt (see TerrainChunk::wants_collision).
}

void TerrainSystem::release_distant_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                             const coopa::scene::FrameContext& ctx) {
    const std::int32_t keep = terrain.params.view_radius + 1;
    for (auto it = terrain.chunks().begin(); it != terrain.chunks().end();) {
        TerrainChunk& chunk = it->second;
        if (chunk.state == ChunkState::Retiring || chunk.state == ChunkState::Dormant ||
            chunk_distance(chunk.coord, centre) <= keep) {
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

        if (chunk.state == ChunkState::Live && terrain.dormant_chunks > 0) {
            // Kept whole, off the tree: the camera that just left this chunk behind is very
            // often about to come back to it (see the file doc on letting go).
            chunk.parked = detach_chunk_object_(terrain, chunk);
            chunk.state  = ChunkState::Dormant;
            ++it;
            continue;
        }

        destroy_chunk_object_(terrain, chunk);
        ++chunks_released_;
        it = terrain.chunks().erase(it);
    }
}

void TerrainSystem::evict_dormant_chunks_(TerrainComponent& terrain) {
    const std::size_t cap = terrain.dormant_chunks > 0 ? static_cast<std::size_t>(terrain.dormant_chunks) : 0;
    pending_.clear();
    for (const auto& entry : terrain.chunks()) {
        if (entry.second.state == ChunkState::Dormant) pending_.push_back(entry.first);
    }
    if (pending_.size() <= cap) return;

    // Oldest first: the chunk the camera left longest ago is the one least likely to be wanted.
    // The coordinate tie-break keeps a frame's eviction set deterministic.
    std::sort(pending_.begin(), pending_.end(), [&terrain](const ChunkCoord& a, const ChunkCoord& b) {
        const std::uint64_t fa = terrain.chunks().at(a).last_used_frame;
        const std::uint64_t fb = terrain.chunks().at(b).last_used_frame;
        if (fa != fb) return fa < fb;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    for (std::size_t i = 0, drop = pending_.size() - cap; i < drop; ++i) {
        TerrainChunk& chunk = terrain.chunks().at(pending_[i]);
        destroy_chunk_object_(terrain, chunk);
        ++chunks_released_;
        terrain.chunks().erase(pending_[i]);
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
        ++chunks_released_;
        it = terrain.chunks().erase(it);
    }
}

void TerrainSystem::release_terrain_(TerrainComponent& terrain, const coopa::scene::FrameContext& ctx) {
    for (auto& entry : terrain.chunks()) {
        TerrainChunk& chunk = entry.second;
        if (chunk.state == ChunkState::Meshing) {
            // Same handling as release_distant_chunks_()'s Meshing branch: the JobHandle must
            // still be close()d exactly once, so this chunk is parked Retiring and drained by
            // advance_retiring_() rather than torn down here.
            if (ctx.jobs != nullptr) ctx.jobs->cancel(chunk.job);
            chunk.state = ChunkState::Retiring;
            chunk.build.reset();
            continue;
        }
        if (chunk.state == ChunkState::Retiring) continue;   // already draining
        destroy_chunk_object_(terrain, chunk);
        ++chunks_released_;
    }
    for (auto it = terrain.chunks().begin(); it != terrain.chunks().end();) {
        if (it->second.state == ChunkState::Retiring) ++it;
        else it = terrain.chunks().erase(it);
    }
    if (!terrain.chunks().empty()) releasing_.insert(&terrain);
}

void TerrainSystem::upload_finished_chunks_(TerrainComponent& terrain, coopa::scene::Scene& scene) {
    last_upload_ms_ = 0.0;
    finished_.clear();
    for (const auto& entry : terrain.chunks()) {
        const TerrainChunk& chunk = entry.second;
        if (chunk.state == ChunkState::Meshing && chunk.job.is_complete()) finished_.push_back(entry.first);
    }
    if (finished_.empty()) return;

    // Soonest-wanted first, whichever way "soonest" means for this terrain's shape.
    order_chunks_(terrain, finished_);

    const auto   start  = std::chrono::steady_clock::now();
    const double budget = std::max(0.1f, terrain.upload_budget_ms);
    for (std::size_t i = 0; i < finished_.size(); ++i) {
        TerrainChunk& chunk = terrain.chunks().at(finished_[i]);

        chunk.job.close();
        // Cleared, not merely closed: a closed handle's pool slot is recycled, so leaving
        // the stale token in place would let ~TerrainComponent()'s drain close it a second
        // time -- which the CounterPool asserts on, by design.
        chunk.job = coopa::job::JobHandle{};
        chunk.state = ChunkState::Live;
        // Every build re-meshes, so this is current whichever branch below runs. A coarse build
        // (ChunkBuildJob::coarse_only) does not call mesh_chunk_styled() at all, but
        // mesh_chunk_field() writes `max_steps` too -- so the horizon cull still gets this
        // chunk's real height either way, which is what it bounds the limb margin with.
        if (chunk.build) chunk.max_steps = chunk.build->mesh.max_steps;
        if (chunk.build && !chunk.build->collision_only) {
            // What is actually on the GPU now, rather than what the policy last asked for.
            chunk.coarse = chunk.build->coarse_only;
            chunk.detail_frame = frame_;
        }

        // A collider-only pass changes nothing that is drawn, so it touches nothing that is:
        // the shape goes onto the object the chunk is already on the tree with.
        if (chunk.build && chunk.build->collision_only) {
            attach_chunk_collision_(terrain, scene, chunk);
            terrain.recycle_build_job(std::move(chunk.build));
            chunk.build.reset();
            last_upload_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (last_upload_ms_ >= budget) break;
            continue;
        }

        // An empty chunk is a real outcome, not a failure: a chunk of open ocean below the
        // waterline emits no faces at all. It stays Live with no object, so it is never
        // re-queued and never draws.
        // A rebuild replaces the chunk's object in this same frame, so it never shows a hole --
        // in the ground. Its water children are bare WaterBodies that WaterSystem bakes over a
        // frame or two, so rebuilding them WOULD show one, and the commonest rebuild of all (a
        // chunk crossing `collision_radius`, sync_chunk_collision_()) does not change the water
        // at all. Unchanged water is therefore carried across, already baked and drawing.
        const std::uint64_t water_hash = chunk.build ? chunk.build->water_hash : 0;
        std::vector<std::unique_ptr<coopa::scene::SceneObject>> kept_water;
        if (chunk.object != nullptr && water_hash != 0 && water_hash == chunk.water_hash) {
            kept_water = take_chunk_water_(chunk);
        }
        chunk.water_hash = water_hash;
        destroy_chunk_object_(terrain, chunk);
        if (chunk.build && (chunk.build->gpu_mesh || !chunk.build->water_still.empty() ||
                            !chunk.build->water_flowing.empty())) {
            create_chunk_object_(terrain, scene, chunk, std::move(kept_water));
        }
        terrain.recycle_build_job(std::move(chunk.build));
        chunk.build.reset();

        last_upload_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Checked after the first chunk, never before it: a budget too small for one chunk
        // would otherwise stall the stream outright.
        if (last_upload_ms_ >= budget) break;
    }
}

void TerrainSystem::submit_queued_chunks_(TerrainComponent& terrain, const ChunkCoord& centre,
                           const coopa::scene::FrameContext& ctx) {
    int in_flight = 0;
    pending_.clear();
    for (auto& entry : terrain.chunks()) {
        const TerrainChunk& c = entry.second;
        if (c.state == ChunkState::Meshing) {
            ++in_flight;
        } else if (c.state == ChunkState::Queued ||
                   (c.state == ChunkState::Live &&
                    (c.stale || c.wants_collision || c.coarse != c.want_coarse))) {
            // The detail mismatch joins the same queue under the same budget and the same
            // nearest-first ordering, so a promotion near the camera naturally beats a demotion
            // on the limb -- no second budget to tune against this one.
            pending_.push_back(entry.first);
        }
    }
    if (pending_.empty()) return;

    // How much work may be outstanding, rather than how much may be started this frame: the
    // JobEngine's business is scheduling it, and a small per-frame count simply leaves its
    // workers idle. A chunk meshed but not yet published still counts, so a frame that cannot
    // afford to publish also stops asking for more.
    const int cap = terrain.max_chunks_in_flight > 0
                        ? terrain.max_chunks_in_flight
                        : static_cast<int>(2u * std::max(1u, ctx.jobs->worker_count()));
    const int budget = std::min(std::max(1, terrain.max_chunk_jobs_per_frame), cap - in_flight);
    if (budget <= 0) return;

    // Soonest first, so the ground under the camera appears before the horizon does.
    order_chunks_(terrain, pending_);

    const int submitted_count = std::min<int>(budget, static_cast<int>(pending_.size()));
    for (int i = 0; i < submitted_count; ++i) {
        TerrainChunk& chunk = terrain.chunks().at(pending_[static_cast<std::size_t>(i)]);

        auto build = terrain.take_build_job();
        // Sampled here, on the owner thread: the job meshes its own copy, so the tiles may be
        // edited while it runs.
        sample_chunk_columns(terrain.field, terrain.params, chunk.coord, build->pad,
                             terrain.topology.get());
        build->library   = &terrain.library();
        // Borrowed for the job's lifetime: the component owns it and the streamer waits for
        // every in-flight job before letting go. See chunk_shaper.h.
        build->shaper    = terrain.shaper.get();
        build->params    = terrain.params;
        build->coord     = chunk.coord;
        build->collision = terrain_wants_collision(terrain, chunk.coord, centre);
        // A Live chunk that is only short of a collider keeps everything it is drawing: the job
        // stops once the collision mesh is built, and upload_finished_chunks_() hangs that on
        // the object already on the tree. Edited tiles (`stale`) invalidate the surface itself
        // and take the full rebuild instead.
        build->collision_only = chunk.state == ChunkState::Live && chunk.wants_collision &&
                                !chunk.stale && build->collision;
        // Resolved here rather than in the policy, so a terrain that turns collision ON gets the
        // full build: a collider must be the chunk's real shape, not a quantised stand-in that
        // changes with where the camera is.
        build->coarse_only = chunk.want_coarse && !build->collision;
        // The job builds the chunk's GPU mesh as well as its geometry (see terrain_chunk.h):
        // all that is left for the frame it lands in is publishing it.
        build->device    = &device_;
        build->allocator = &allocator_;

        chunk.build           = build;
        chunk.job             = ctx.jobs->create_handle();
        chunk.state           = ChunkState::Meshing;
        chunk.stale           = false;
        chunk.wants_collision = false;
        chunk.has_collision   = build->collision;

        // One job per chunk rather than one parallel_for over all of them: chunks finish and
        // appear individually, and a chunk that leaves view mid-flight can be cancelled on
        // its own without disturbing its neighbours. The priority is what gets the ground
        // under the camera built first whatever order this queue was in -- and what keeps the
        // far ring from competing with the frame's own parallel work.
        const coopa::job::Priority priority = chunk_job_priority_(terrain, chunk.coord);
        ctx.jobs->submit([build]() { build->run(); }, k_terrain_job_type, chunk.job, nullptr, 0u, priority);
    }
}

void TerrainSystem::create_chunk_object_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                          TerrainChunk& chunk,
                          std::vector<std::unique_ptr<coopa::scene::SceneObject>> kept_water) {
    if (terrain.owner == nullptr) return;

    ++chunk.mesh_serial;
    auto object = std::make_unique<coopa::scene::SceneObject>(chunk_object_name_(chunk.coord));
    // System-created and never saved: the tiles are the terrain, and this is only their current
    // geometry. The marker is inherited by the water children, puts the chunk under the
    // Hierarchy's "Runtime" group, and is how consumers that read the terrain through its field
    // (GroundProbe) know to skip its mesh -- a chunk's box top is not its surface.
    object->add_component<toy::scene::RuntimeObject>("TileTerrain", "terrain chunk -- edit the terrain's tiles");
    auto* transform = object->add_component<TransformComponent>();
    transform->transform().set_position(chunk_origin(terrain.params, chunk.coord, terrain.shaper.get()));
    if (kept_water.empty()) {
        add_chunk_water_(terrain, *object, chunk.build->water_still, false);
        add_chunk_water_(terrain, *object, chunk.build->water_flowing, true);
    } else {
        // Re-parented, not re-built: each keeps its WaterBody, its baked query and its
        // published tile meshes, so it draws in this frame exactly as it did in the last. The
        // new chunk transform holds the same chunk_origin(), so the body's world matrix is
        // unchanged and WaterSystem does not count it as moved.
        for (auto& water : kept_water) {
            if (auto* tc = water->get_transform()) tc->set_parent_transform(&transform->transform());
            object->add_child(std::move(water));
        }
    }
    if (!chunk.build->gpu_mesh) {
        chunk.object = terrain.owner->add_child(std::move(object));
        scene.adopt(*chunk.object);
        return;
    }

    // Built on the chunk's worker, not here (see terrain_chunk.h): publishing it is all this
    // thread does with it.
    chunk.mesh = std::move(chunk.build->gpu_mesh);
    ++chunks_built_;
    const std::string id = chunk_asset_id_(terrain, chunk);
    auto handle = assets_.create<Mesh>(id, chunk.mesh);

    auto* renderer = object->add_component<MeshRenderer>();
    renderer->material = terrain.material;
    // Chunks carry blend codes (encode_surface_blend()), not texture coordinates:
    // terrain_styled.frag picks each pixel's kind from them, takes its colour from the atlas and
    // adds world-space detail -- it needs the atlas grid and the cell size.
    // A terrain whose geometry was shaped off the world plane names its own build of the shader
    // (TerrainParams::surface_shader): "up" is then the per-vertex uv.y the shaping wrote.
    renderer->material.shader        = terrain.params.surface_shader.empty()
                                           ? "terrain_styled"
                                           : terrain.params.surface_shader;
    renderer->material.shader_params = glm::vec4(static_cast<float>(k_atlas_columns), static_cast<float>(k_atlas_rows),
                                                 terrain.params.height_step, terrain.params.tile_size);
    // The surface detail: one baked, tiling, mipmapped texture shared by every chunk, bound in
    // the material set's normal slot (see terrain_styled.glsl, which redefines that slot's
    // sampling and neutralises the backbone's normal decode). Assigned as a HANDLE rather than
    // a `texture_normal` path: the texture is baked at runtime, not loaded from a file, so it
    // has no path to resolve -- the handle is what has_normal_map() and the texture cache key on.
    renderer->material.normal_handle = detail_texture_;
    // Probe / GI bakes draw with stock shaders only and would misread those codes.
    renderer->affects_reflection_probes = false;
    // Far chunks out of the cascades: a chunk's whole vertex count is drawn into every shadow
    // view whose frustum it touches, and the resident set reaches far past where its shadow
    // could be seen. 0 (the default) keeps every loaded chunk in, as before.
    renderer->shadow_max_distance = terrain.shadow_distance;
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

void TerrainSystem::attach_chunk_collision_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                                            TerrainChunk& chunk) {
    // has_collision went false again: the chunk left the radius while its job ran, and the shape
    // is simply dropped. An empty chunk (no object, no geometry) satisfies the request with no
    // collider at all, which is what has_collision already records.
    if (!chunk.has_collision || chunk.object == nullptr || !chunk.build->collision_mesh) return;
    if (chunk.object->get_component<coopa::physx::components::MeshCollider>() != nullptr) return;
    auto* collider = chunk.object->add_component<coopa::physx::components::MeshCollider>();
    collider->set_mesh(chunk.build->collision_mesh);
    if (!terrain.physics_material.empty()) {
        collider->set_material_asset(assets_.load<coopa::physx::dynamics::PhysicsMaterial>(
            "physics_materials/" + terrain.physics_material + ".yaml"));
    }
    // add_component() does not hand the component the Scene; adopt() is what create_chunk_object_()
    // relies on for the same reason.
    scene.adopt(*chunk.object);
    physics_dirty_ = true;
}

std::vector<std::unique_ptr<coopa::scene::SceneObject>> TerrainSystem::take_chunk_water_(TerrainChunk& chunk) {
    std::vector<std::unique_ptr<coopa::scene::SceneObject>> taken;
    if (chunk.object == nullptr) return taken;
    // Two passes: detaching edits the child list being walked.
    std::vector<coopa::scene::SceneObject*> water;
    for (const auto& child : chunk.object->children()) {
        if (child->get_component<toy::water::WaterBody>() != nullptr) water.push_back(child.get());
    }
    taken.reserve(water.size());
    for (coopa::scene::SceneObject* child : water) {
        if (auto owned = chunk.object->detach_child(child)) taken.push_back(std::move(owned));
    }
    return taken;
}

void TerrainSystem::add_chunk_water_(const TerrainComponent& terrain, coopa::scene::SceneObject& chunk_object,
                                     const ChunkWaterData& water, bool flowing) {
    if (water.empty()) return;
    // A child of the chunk: placed by the chunk's transform, and destroyed with it. The water
    // system bakes it (depth from the beds below, flow from its slope and `u`) and draws it.
    auto object = std::make_unique<coopa::scene::SceneObject>(flowing ? "river" : "water");
    auto* tc = object->add_component<TransformComponent>();
    // add_child() does not link transforms; the water is placed in the chunk's space.
    if (auto* parent = chunk_object.get_transform()) tc->set_parent_transform(&parent->transform());
    auto* body = object->add_component<water::WaterBody>();
    water::copy_water_settings(terrain.water_look, *body);
    if (water.local_frame) {
        // A shaped chunk's: placed in its own frame (ChunkWaterData::frame), and worked on in it.
        tc->transform().set_position(glm::vec3(water.frame[3]));
        tc->transform().set_rotation_quat(glm::quat_cast(glm::mat3(water.frame)));
        body->local_frame = true;
        // A small world's sea is seen whole, where every ocean pixel is deep enough to read as
        // fully opaque (water_surface.glsl's WATER_LOCAL_FRAME branch). The engine-wide default
        // is a dark teal tuned for shallow lakes, which at full opacity reads near-black against
        // space, so an ocean blue is the better default out here. Only when the scene left the
        // colour alone -- an authored `water:` block on the terrain still wins.
        if (body->deep_color == water::WaterBody{}.deep_color) {
            body->deep_color = glm::vec3(0.03f, 0.15f, 0.28f);
        }
    }
    body->mode = flowing ? water::WaterMode::Flowing : water::WaterMode::Planar;
    // Every body over a chunk keeps the SAME wave field, rivers included. A river and the lake
    // it runs into share their seam's vertices (build_chunk_water()), and the vertex stage
    // displaces a vertex from the body's own wave params -- so two bodies with different waves
    // pull a shared vertex apart and the bed shows through the gap between them. A river's own
    // character comes from its per-vertex flow and turbulence (the current's ripples and
    // whitewater), which is per vertex and agrees across a seam by construction.
    body->set_geometry(water.positions, water.uvs, water.indices, water.depths, water.wave_scales);
    chunk_object.add_child(std::move(object));
}

std::unique_ptr<coopa::scene::SceneObject> TerrainSystem::detach_chunk_object_(TerrainComponent& terrain,
                                                                               TerrainChunk& chunk) {
    std::unique_ptr<coopa::scene::SceneObject> owned;
    if (chunk.parked) {
        owned = std::move(chunk.parked);           // already off the tree
    } else if (chunk.object != nullptr && terrain.owner != nullptr) {
        owned = terrain.owner->detach_child(chunk.object);
    }
    chunk.object = nullptr;
    if (owned) {
        if (auto* collider = owned->get_component<coopa::physx::components::MeshCollider>()) {
            // The physics world holds a raw pointer to the shape until its next refresh(), and
            // an async navmesh bake may hold its snapshot longer -- so park the shape too.
            terrain.retire_collision(collider->owned_mesh(), frame_);
            physics_dirty_ = true;
        }
    }
    return owned;
}

void TerrainSystem::destroy_chunk_object_(TerrainComponent& terrain, TerrainChunk& chunk) {
    // The returned unique_ptr is destroyed with this statement, which destroys the MeshRenderer
    // and with it the last AssetHandle on the chunk's mesh -- the refcount unload() requires to
    // have reached zero.
    detach_chunk_object_(terrain, chunk);
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
