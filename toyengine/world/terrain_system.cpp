#include <toyengine/world/terrain_system.h>

#include <toyengine/water/water_body.h>

#include <chrono>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

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
#include <toyengine/world/terrain_detail_texture.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_map_data.h>
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

void TerrainSystem::execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) {
    ++frame_;
    physics_dirty_ = false;
    wanted_chunks_ = 0;
    unfinished_chunks_ = 0;
    terrain_baking_ = false;
    // Baked on the first frame that has terrain, not in the constructor: the bake submits a
    // one-shot upload, and a system is constructed before the device is necessarily ready to
    // take one. It registers itself with the asset manager, so this is one upload per run.
    if (!detail_texture_.is_loaded() && !scene.get_components<TerrainComponent>().empty()) {
        coopa::gfx::command::CommandPool pool(device_, device_.graphics_family(), true);
        detail_texture_ = bake_terrain_detail_texture(device_, allocator_, pool, assets_, 512,
                                                      16.0f, ctx.jobs);
    }
    for (TerrainComponent* terrain : scene.get_components<TerrainComponent>()) {
        // Not ready yet means the world bake or the tile-set bake is still running, and no chunk
        // has been asked for -- a zero wanted count that must not read as "nothing left to do".
        if (!terrain->is_ready()) terrain_baking_ = true;
        tick_terrain_(*terrain, scene, ctx);
    }
    sync_planet_gravity_(scene);
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

    advance_water_settles_(terrain);
    mark_edited_chunks_(terrain);
    if (terrain.params.planet) {
        // A planet is small and seen whole: every chunk stays loaded, nearest the camera first.
        want_planet_chunks_(terrain);
        upload_finished_chunks_(terrain, scene);
        submit_queued_chunks_(terrain, ChunkCoord{0, 0}, ctx);
        cull_planet_horizon_(terrain);
        return;
    }
    const ChunkCoord centre = latched_centre_(terrain);
    want_chunks_(terrain, centre, scene);
    release_distant_chunks_(terrain, centre, ctx);
    evict_dormant_chunks_(terrain);
    upload_finished_chunks_(terrain, scene);
    submit_queued_chunks_(terrain, centre, ctx);
}

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
    if (terrain.tiles && !terrain.tiles->empty()) {
        const float slack = terrain.params.chunk_world_size();
        const float w = static_cast<float>(terrain.tiles->width()) * terrain.params.tile_size;
        const float h = static_cast<float>(terrain.tiles->height()) * terrain.params.tile_size;
        if (pos.x > -slack && pos.y > -slack && pos.x < w + slack && pos.y < h + slack) return pos;

        // Off the map: stream around what it looks at instead, so an orbit or editor camera
        // parked outside the world still loads the part of it that it is framing.
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
        if (terrain.params.planet) {
            // A tile on a face's edge is in the next face's skirt too: mark the chunks of every
            // tile the rect touches and of each of their neighbours, wrapped across the faces.
            const PlanetLayout layout = terrain.params.layout();
            const auto mark = [&](const glm::ivec2& t) {
                const ChunkCoord c{t.x / size, t.y / size};
                auto it = terrain.chunks().find(c);
                if (it != terrain.chunks().end() && it->second.state != ChunkState::Retiring &&
                    it->second.state != ChunkState::Queued) it->second.stale = true;
            };
            for (int y = std::max(0, rect.y0); y < std::min(tiles.height(), rect.y1); ++y)
                for (int x = std::max(0, rect.x0); x < std::min(tiles.width(), rect.x1); ++x)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) mark(layout.neighbour(x, y, dx, dy));
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
    terrain.applied_tile_revision = tiles.revision();
}

void TerrainSystem::want_planet_chunks_(TerrainComponent& terrain) {
    if (!terrain.tiles) return;
    const std::int32_t size = std::max(1, terrain.params.chunk_size);
    for (std::int32_t cy = 0; cy * size < terrain.tiles->height(); ++cy) {
        for (std::int32_t cx = 0; cx * size < terrain.tiles->width(); ++cx) {
            const ChunkCoord coord{cx, cy};
            if (terrain.chunks().find(coord) != terrain.chunks().end()) continue;
            TerrainChunk chunk;
            chunk.coord = coord;
            chunk.state = ChunkState::Queued;
            terrain.chunks().emplace(coord, std::move(chunk));
        }
    }
}

void TerrainSystem::advance_water_settles_(TerrainComponent& terrain) {
    if (terrain.water_settles.empty() || !terrain.tiles) return;
    // One budget shared over however many settles are in flight, oldest first, so digging again
    // while the last flood is still running cannot multiply the per-frame cost.
    std::size_t budget = static_cast<std::size_t>(std::max(1, terrain.water_settle_budget));
    while (!terrain.water_settles.empty() && budget > 0) {
        WaterSettle& settle = terrain.water_settles.front();
        const std::size_t before = settle.spent;
        step_settle(*terrain.tiles, settle, budget);
        budget -= std::min(budget, settle.spent - before);
        // Whatever it filled is journalled, so mark_edited_chunks_() -- running straight after
        // this -- stales exactly the chunks the water reached and they re-mesh with it.
        if (settle.done()) terrain.water_settles.pop_front();
    }
}

void TerrainSystem::cull_planet_horizon_(TerrainComponent& terrain) {
    if (!terrain.params.planet || terrain.chunks().empty()) return;
    CameraComponent* camera = CameraComponent::main();
    if (camera == nullptr || camera->owner == nullptr || camera->owner->get_transform() == nullptr) return;

    glm::mat4 to_local(1.0f);
    if (terrain.owner != nullptr) {
        if (auto* tc = terrain.owner->get_transform()) to_local = glm::inverse(tc->transform().get_world_matrix());
    }
    const glm::vec3 eye = glm::vec3(to_local * camera->owner->get_transform()->transform().get_world_matrix()[3]);
    const float distance = glm::length(eye);
    const PlanetLayout layout = terrain.params.layout();
    const float radius = layout.radius();
    // Inside the planet, or so near the surface that the horizon is on top of the camera: draw
    // everything and let the frustum do the work. (It also keeps a camera at the surface, where
    // radius / distance is ~1, from hiding the ground it is standing on.)
    if (distance <= radius * 1.02f) {
        for (auto& [coord, chunk] : terrain.chunks()) {
            if (chunk.object != nullptr) chunk.object->set_active(true);
        }
        return;
    }

    const glm::vec3 view = eye / distance;
    // How far round the globe anything can still be seen from here, as an angle from the camera's
    // own direction: `alpha` to the horizon itself (where the view grazes the sphere), plus
    // `beta` for the terrain standing on it -- a peak `height_scale` tall sits on the tangent
    // line that much further round -- plus half a chunk's angular width, since the test below
    // asks about a chunk's CENTRE and its near edge reaches closer.
    const float alpha = std::acos(std::clamp(radius / distance, -1.0f, 1.0f));
    const float beta = std::acos(std::clamp(radius / (radius + std::max(0.0f, terrain.params.height_scale)), -1.0f, 1.0f));
    const float chunk_angle = 0.75f * terrain.params.chunk_world_size() / radius;
    const float limit = std::cos(std::min(alpha + beta + chunk_angle, 3.14159265f));
    // Hidden a little further round than it is shown again, so a chunk sitting on the limb of a
    // camera that is barely moving cannot toggle every frame.
    const float hide_below = limit - 0.02f;
    const float show_above = limit;

    const std::int32_t size = std::max(1, terrain.params.chunk_size);
    for (auto& [coord, chunk] : terrain.chunks()) {
        if (chunk.object == nullptr) continue;
        // The chunk's centre, as a direction out of the planet's centre.
        const glm::vec3 centre(layout.tile_direction(coord.x * size + size / 2, coord.y * size + size / 2));
        const float facing = glm::dot(centre, view);
        const bool visible = chunk.object->active();
        if (visible && facing < hide_below) chunk.object->set_active(false);
        else if (!visible && facing > show_above) chunk.object->set_active(true);
    }
}

void TerrainSystem::sync_planet_gravity_(coopa::scene::Scene& scene) {
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    if (physics == nullptr) return;
    coopa::physx::PhysicsWorld& world = physics->world();
    std::unordered_map<const TerrainComponent*, std::uint32_t> registered;
    for (TerrainComponent* terrain : scene.get_components<TerrainComponent>()) {
        if (!terrain->params.planet || terrain->gravity <= 0.0f) continue;
        coopa::physx::PhysicsWorld::GravitySource source;
        source.centre = terrain->owner && terrain->owner->get_transform()
                            ? glm::vec3(terrain->owner->get_transform()->transform().get_world_matrix()[3])
                            : glm::vec3(0.0f);
        source.radius = terrain->params.layout().radius();
        // No pull until every chunk (and so all the planet's ground) is in: anything dropped on
        // it at the start would otherwise fall through ground that is still being built.
        const int cs = std::max(1, terrain->params.chunk_size);
        const int per_edge = (std::max(1, terrain->params.face_tiles) + cs - 1) / cs;
        int live = 0;
        for (const auto& [coord, chunk] : terrain->chunks()) live += chunk.state == ChunkState::Live ? 1 : 0;
        if (live >= 6 * per_edge * per_edge) planet_ready_.insert(terrain);
        source.strength = planet_ready_.count(terrain) ? terrain->gravity : 0.0f;
        source.range = source.radius * 8.0f;
        const auto found = planet_gravity_.find(terrain);
        if (found != planet_gravity_.end() && world.update_gravity_source(found->second, source)) {
            registered.emplace(terrain, found->second);
        } else {
            registered.emplace(terrain, world.add_gravity_source(source));
        }
    }
    for (const auto& [terrain, id] : planet_gravity_) {
        if (registered.find(terrain) == registered.end()) {
            world.remove_gravity_source(id);
            planet_ready_.erase(terrain);
        }
    }
    planet_gravity_ = std::move(registered);
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

void TerrainSystem::upload_finished_chunks_(TerrainComponent& terrain, coopa::scene::Scene& scene) {
    last_upload_ms_ = 0.0;
    finished_.clear();
    for (const auto& entry : terrain.chunks()) {
        const TerrainChunk& chunk = entry.second;
        if (chunk.state == ChunkState::Meshing && chunk.job.is_complete()) finished_.push_back(entry.first);
    }
    if (finished_.empty()) return;

    // Nearest the latched centre first: in hash order the chunk under the camera waits behind
    // the horizon's whenever the budget bites.
    const ChunkCoord centre = terrain.stream_centre.centre;
    std::sort(finished_.begin(), finished_.end(), [&centre](const ChunkCoord& a, const ChunkCoord& b) {
        const std::int32_t da = chunk_distance(a, centre);
        const std::int32_t db = chunk_distance(b, centre);
        if (da != db) return da < db;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });

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
                   (c.state == ChunkState::Live && (c.stale || c.wants_collision))) {
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

        auto build = terrain.take_build_job();
        // Sampled here, on the owner thread: the job meshes its own copy, so the tiles may be
        // edited while it runs.
        sample_chunk_columns(*terrain.tiles, terrain.params, chunk.coord, build->pad);
        build->library   = &terrain.library();
        build->params    = terrain.params;
        build->coord     = chunk.coord;
        build->collision = terrain_wants_collision(terrain, chunk.coord, centre);
        // A Live chunk that is only short of a collider keeps everything it is drawing: the job
        // stops once the collision mesh is built, and upload_finished_chunks_() hangs that on
        // the object already on the tree. Edited tiles (`stale`) invalidate the surface itself
        // and take the full rebuild instead.
        build->collision_only = chunk.state == ChunkState::Live && chunk.wants_collision &&
                                !chunk.stale && build->collision;
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
        // A planet has no centre to be near: every one of its chunks is wanted at once.
        const coopa::job::Priority priority =
            terrain.params.planet ? coopa::job::Priority::Normal
                                  : chunk_priority(chunk_distance(chunk.coord, centre), terrain.params.view_radius);
        ctx.jobs->submit([build]() { build->run(); }, k_terrain_job_type, chunk.job, nullptr, 0u, priority);
    }
}

void TerrainSystem::create_chunk_object_(TerrainComponent& terrain, coopa::scene::Scene& scene,
                          TerrainChunk& chunk,
                          std::vector<std::unique_ptr<coopa::scene::SceneObject>> kept_water) {
    if (terrain.owner == nullptr) return;

    ++chunk.mesh_serial;
    auto object = std::make_unique<coopa::scene::SceneObject>(chunk_object_name_(chunk.coord));
    auto* transform = object->add_component<TransformComponent>();
    transform->transform().set_position(chunk_origin(terrain.params, chunk.coord));
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
    // A planet's chunks use the shader's planet build (terrain_styled.glsl): their "up" is the
    // per-vertex uv.y the warp wrote, not world +Z.
    renderer->material.shader        = terrain.params.planet ? "terrain_styled_planet" : "terrain_styled";
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
        // A planet's: placed in its own frame (ChunkWaterData::frame), and worked on in it.
        tc->transform().set_position(glm::vec3(water.frame[3]));
        tc->transform().set_rotation_quat(glm::quat_cast(glm::mat3(water.frame)));
        body->local_frame = true;
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
