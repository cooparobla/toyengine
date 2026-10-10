#include <toyengine/world/terrain_component.h>

#include <coopa/asset/asset_handle.h>
#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_task.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>
#include <toyengine/world/terrain_sampler.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_topology.h>
#include <toyengine/world/tile_types.h>

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
}

void TerrainComponent::begin_generation(coopa::job::JobEngine* jobs) {
    // Remembered even on the early-out path: the destructor needs it to drain chunk jobs,
    // and those outlive generation by the whole life of the scene.
    jobs_ = jobs;
    if (generator_ || sampler_.is_built()) return;

    coopa::maps::MapConfig config;
    config.seed              = seed;
    config.grid_size         = grid_size;
    config.sea_level         = sea_level;
    config.terrain_roughness = terrain_roughness;
    config.river_count       = river_count;
    config.shape.shape            = coopa::maps::map_shape_from_name(shape);
    config.shape.continent_count  = continent_count;
    config.shape.continent_size_m = continent_size_m;
    config.shape.irregularity     = irregularity;
    config.shape.coast_detail     = coast_detail;
    config.temperature_offset     = temperature_offset;
    map_config_              = config;

    generator_ = std::make_unique<coopa::maps::MapGenerator>(config, logger_);
    generator_->set_job_engine(jobs);
    // The generator must outlive the task -- the job writes into its graph -- and it does:
    // both are members, and task_ is declared after generator_ so it destructs first.
    task_ = generator_->generate_async();
}

bool TerrainComponent::poll_generation() {
    if (!generator_ || sampler_.is_built() || !task_.done()) return false;

    sampler_.build(map_config_, std::move(generator_->graph()), params);
    // The graph is moved into the sampler; the generator has nothing left to own, and
    // dropping it also releases the task's job handle through task_'s own destructor.
    task_ = coopa::maps::MapTask{};
    generator_.reset();
    return sampler_.is_built();
}

void TerrainComponent::set_style_piece_source(std::size_t style, TilePiece piece,
                            coopa::asset::AssetHandle<SkinnedMeshSource> handle) {
    if (style_sources_.size() <= style) style_sources_.resize(style + 1);
    style_sources_[style][static_cast<std::size_t>(piece)] = std::move(handle);
}

bool TerrainComponent::poll_side_meshes() {
    if (library_.is_baked()) return false;
    if (!side_source_.is_loaded()) {
        // A mistyped side_mesh path would otherwise produce a world that is simply never
        // built, with nothing on screen and nothing in the log to say why -- the most
        // expensive kind of silence. Said once, not once per frame.
        if (side_source_.is_failed() && !warned_side_mesh_) {
            warned_side_mesh_ = true;
            logger_.error("side mesh '" + side_mesh + "' failed to load: " +
                          side_source_.error() + " -- no terrain will be built.");
        }
        return false;
    }

    // Styled pieces must all have settled (loaded or failed) before anything bakes: chunk
    // jobs start the moment is_baked() turns true, and a library must not change under them.
    for (const auto& pieces : style_sources_) {
        for (const auto& handle : pieces) {
            if (handle.is_valid() && !handle.is_loaded() && !handle.is_failed()) return false;
        }
    }

    library_.bake_canonical(*side_source_.get());
    for (std::size_t i = 0; i < k_tile_face_count; ++i) {
        if (face_sources_[i].is_loaded()) {
            library_.bake_side(static_cast<TileFace>(i), *face_sources_[i].get());
        }
    }
    bake_styles_();
    return library_.is_baked();
}

void TerrainComponent::bake_styles_() {
    for (std::size_t s = 0; s < styles.size(); ++s) {
        const std::uint8_t index = library_.add_style();
        if (s >= style_sources_.size()) continue;
        for (std::size_t p = 0; p < k_tile_piece_count; ++p) {
            const auto& handle = style_sources_[s][p];
            if (handle.is_loaded()) {
                library_.bake_style_piece(index, static_cast<TilePiece>(p), *handle.get());
            } else if (handle.is_failed()) {
                logger_.warn("tile style '" + styles[s].name + "': piece '" +
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
