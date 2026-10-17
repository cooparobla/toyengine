#include <toyengine/world/terrain_component.h>

#include <coopa/asset/asset_handle.h>
#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_task.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <toyengine/world/terrain_sampler.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_topology.h>
#include <toyengine/world/tile_types.h>

#include <cmath>

namespace toy {
namespace world {

TerrainComponent::~TerrainComponent() {
    if (jobs_ == nullptr) return;
    for (auto& entry : chunks_) {
        coopa::job::JobHandle& handle = entry.second.job;
        if (!handle.is_valid()) continue;
        jobs_->cancel(handle);   // stops whatever has not started
        jobs_->wait_for(handle); // ...and blocks on whatever has
        handle.close();
    }
    if (bake_handle_.is_valid()) {
        // The bake job reads sampler_: it must finish before the sampler goes.
        jobs_->wait_for(bake_handle_);
        bake_handle_.close();
    }
}

bool terrain_wants_collision(const TerrainComponent& terrain, const ChunkCoord& coord,
                             const ChunkCoord& centre) {
    if (!terrain.collision) return false;
    if (terrain.params.planet || terrain.collision_radius <= 0) return true;
    return chunk_distance(coord, centre) <= terrain.collision_radius;
}

std::shared_ptr<ChunkBuildJob> TerrainComponent::take_build_job() {
    if (job_pool_.empty()) return std::make_shared<ChunkBuildJob>();
    std::shared_ptr<ChunkBuildJob> job = std::move(job_pool_.back());
    job_pool_.pop_back();
    return job;
}

void TerrainComponent::recycle_build_job(std::shared_ptr<ChunkBuildJob> job) {
    // use_count 1 means this is the last reference: the job itself is gone, so its buffers are
    // nobody else's. A cancelled job may still be running and holding the other one -- let that
    // one be freed by whoever drops it last.
    if (!job || job.use_count() != 1 || job_pool_.size() >= k_job_pool_max) return;
    job->reset_outputs();
    job_pool_.push_back(std::move(job));
}

void TerrainComponent::begin_generation(coopa::job::JobEngine* jobs) {
    // Remembered even on the early-out path: the destructor needs it to drain chunk jobs,
    // and those outlive generation by the whole life of the scene.
    jobs_ = jobs;
    if (!tiles || !tiles->needs_bake() || baking()) return;
    if (started_bake_request_ == tiles->bake_requests()) return;   // this request is done (or failed)
    started_bake_request_ = tiles->bake_requests();

    // The values the CLI derives after reading a config (noise seeds from the master seed, the
    // island field scaled to the grid) -- or one seed would make the same coastline as another.
    // A planet's world is mapcoopa's globe: the same settings, generated on a sphere.
    coopa::maps::MapConfig authored = generator;
    if (params.planet) authored.topology = coopa::maps::MapTopology::Sphere;
    const coopa::maps::MapConfig config = coopa::maps::derived_config(authored);
    map_config_ = config;

    generator_ = std::make_unique<coopa::maps::MapGenerator>(config, logger_);
    generator_->set_job_engine(jobs);
    // The generator must outlive the task -- the job writes into its graph -- and it does:
    // both are members, and task_ is declared after generator_ so it destructs first.
    task_ = generator_->generate_async();
}

bool TerrainComponent::poll_generation() {
    // 1. The map is done: index it, then fill the tiles from it -- on a worker, since a large
    //    world is a million samples.
    if (generator_ && !bake_job_ && task_.done()) {
        sampler_.build(map_config_, std::move(generator_->graph()), params);
        task_ = coopa::maps::MapTask{};
        generator_.reset();
        if (!sampler_.is_built()) return false;
        bake_job_ = std::make_shared<BakeJob>();
        auto job = bake_job_;
        const TerrainSampler* sampler = &sampler_;
        if (jobs_ != nullptr) {
            bake_handle_ = jobs_->create_handle();
            jobs_->submit([job, sampler]() { sampler->bake(job->baked); }, 0x7E44A101u, bake_handle_, nullptr, 0u,
                          coopa::job::Priority::Normal);
        } else {
            sampler->bake(job->baked);
        }
    }
    // 2. The fill is done: the tiles take it (and every chunk re-meshes from it).
    if (bake_job_ && (!bake_handle_.is_valid() || bake_handle_.is_complete())) {
        if (bake_handle_.is_valid()) {
            bake_handle_.close();
            bake_handle_ = coopa::job::JobHandle{};
        }
        if (tiles) tiles->assign_baked(std::move(bake_job_->baked));
        bake_job_.reset();
        return true;
    }
    return false;
}

bool TerrainComponent::tile_at(const glm::vec3& world, glm::ivec2& tile) const {
    if (!tiles || tiles->empty()) return false;
    glm::vec3 local = world;
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) local = glm::vec3(glm::inverse(tc->transform().get_world_matrix()) * glm::vec4(world, 1.0f));
    }
    tile = glm::ivec2(static_cast<int>(std::floor(local.x / params.tile_size)), static_cast<int>(std::floor(local.y / params.tile_size)));
    return tiles->contains(tile.x, tile.y);
}

const TileRecord* TerrainComponent::record_at(const glm::vec3& world) const {
    glm::ivec2 tile;
    return tile_at(world, tile) ? &tiles->at(tile.x, tile.y) : nullptr;
}

float TerrainComponent::height_at(const glm::vec3& world) const {
    const TileRecord* r = record_at(world);
    float z = r ? static_cast<float>(std::max<int>(0, r->steps)) * params.height_step : 0.0f;
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) z += glm::vec3(tc->transform().get_world_matrix()[3]).z;
    }
    return z;
}

std::optional<ChunkState> TerrainComponent::chunk_state_at(const glm::vec3& world) const {
    glm::vec3 local = world;
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) {
            local = glm::vec3(glm::inverse(tc->transform().get_world_matrix()) * glm::vec4(world, 1.0f));
        }
    }
    const auto found = chunks_.find(params.chunk_at(local));
    if (found == chunks_.end()) return std::nullopt;
    return found->second.state;
}

bool TerrainComponent::chunk_loaded_at(const glm::vec3& world) const {
    const std::optional<ChunkState> state = chunk_state_at(world);
    if (!state) return false;
    return *state == ChunkState::Queued || *state == ChunkState::Meshing ||
           *state == ChunkState::Live;
}

water::WaterBody TerrainComponent::default_water_look() {
    water::WaterBody look;
    // A world of one-metre tiles and water a few steps deep: small, short waves, and clear
    // enough that the beds read through the shallows.
    look.waves.amplitude = 0.06f;
    look.waves.wavelength = 3.5f;
    look.waves.steepness = 0.45f;
    look.clarity = 2.5f;
    look.shore_foam_depth = 0.35f;
    look.edge_fade_depth = 0.1f;
    look.ripple_strength = 0.3f;
    look.max_depth = 20.0f;
    return look;
}

void TerrainComponent::set_style_piece_source(std::size_t style, TilePiece piece,
                            coopa::asset::AssetHandle<SkinnedMeshSource> handle) {
    if (style_sources_.size() <= style) style_sources_.resize(style + 1);
    style_sources_[style][static_cast<std::size_t>(piece)] = std::move(handle);
}

bool TerrainComponent::poll_tile_sets() {
    if (library_.is_baked()) return false;

    // Every piece must have settled (loaded or failed) before anything bakes: chunk jobs start
    // the moment is_baked() turns true, and a library must not change under them.
    bool any = false;
    for (const auto& pieces : style_sources_) {
        for (const auto& handle : pieces) {
            if (!handle.is_valid()) continue;
            if (!handle.is_loaded() && !handle.is_failed()) return false;
            any = any || handle.is_loaded();
        }
    }
    if (!any) {
        // A mistyped tile_set path would otherwise produce a world that is simply never built,
        // with nothing on screen and nothing in the log to say why. Said once.
        if (!warned_no_tile_set_) {
            warned_no_tile_set_ = true;
            logger_.error("tile set '" + (styles.empty() ? tile_set : styles.front().prefix) +
                          "' has no piece that loaded -- no terrain will be built.");
        }
        return false;
    }
    bake_styles_();
    return library_.is_baked();
}

void TerrainComponent::retire_collision(std::shared_ptr<const coopa::physx::geometry::TriangleMesh> mesh,
                                        std::uint64_t frame) {
    if (mesh) retired_collision_.emplace_back(frame, std::move(mesh));
}

void TerrainComponent::release_retired_collision(std::uint64_t frame, std::uint64_t keep_frames) {
    while (!retired_collision_.empty() && retired_collision_.front().first + keep_frames < frame) {
        retired_collision_.pop_front();
    }
}

void TerrainComponent::bake_styles_() {
    for (std::size_t s = 0; s < styles.size(); ++s) {
        const std::uint8_t index = library_.add_style();
        library_.set_round_kind_corners(index, styles[s].round_kind_corners);
        if (s >= style_sources_.size()) continue;
        for (std::size_t p = 0; p < k_tile_piece_count; ++p) {
            const auto& handle = style_sources_[s][p];
            if (handle.is_loaded()) {
                library_.bake_style_piece(index, static_cast<TilePiece>(p), *handle.get());
            } else if (handle.is_failed()) {
                logger_.warn("tile set '" + styles[s].name + "': piece '" +
                             tile_piece_name(static_cast<TilePiece>(p)) + "' failed to load: " +
                             handle.error() + " -- falling back to a simpler piece.");
            }
        }
    }
    if (!library_.has_styles()) return;
    library_.finish_styles();
    for (std::size_t k = 0; k < k_tile_kind_count; ++k) {
        const int style = kind_styles[k] >= 0 ? kind_styles[k] : default_style;
        library_.set_kind_style(static_cast<TileKind>(k),
                                static_cast<std::uint8_t>(std::clamp(style, 0, static_cast<int>(styles.size()) - 1)));
    }
}

} // namespace world
} // namespace toy
