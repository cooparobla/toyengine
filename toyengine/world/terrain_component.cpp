#include <toyengine/world/terrain_component.h>

// For a complete HeightmapTerrainRuntime: the constructor and destructor below destroy one.
#include <toyengine/world/heightmap_terrain_system.h>

#include <iostream>

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
#include <toyengine/world/terra_image.h>
#include <toyengine/world/terra_store.h>
#include <toyengine/world/terrain_system.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_topology.h>
#include <toyengine/world/tile_edit.h>
#include <toyengine/world/tile_types.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace toy {
namespace world {

namespace {

/**
 * @brief A ray's hit on a planet's tile columns (terrain-local space, the planet's centre at the
 *        origin): marched from where it enters the highest column's sphere, a fraction of a tile
 *        at a time, until it is inside a column -- then the crossing is refined by bisection.
 *        Columns are tested by their tops only (a wall hit reads as the column in front of it).
 */
bool planet_raycast(const TerraData& d, const TerrainParams& params, const glm::vec3& origin,
                    const glm::vec3& dir, toy::world::TileHit& hit) {
    const PlanetLayout layout = params.layout();
    const double r0 = layout.radius();
    const glm::dvec3 o(origin), v(glm::normalize(glm::dvec3(dir)));
    const auto top = [&](const glm::dvec3& p, glm::ivec2& tile) {
        tile = layout.tile_of(glm::normalize(p));
        return r0 + static_cast<double>(std::max<int>(0, d.get(tile.x, tile.y).steps)) * params.height_step;
    };
    const double outer = r0 + 256.0 * params.height_step;
    // Where the ray meets the outer sphere (|o + t v| = outer).
    const double b = glm::dot(o, v), c = glm::dot(o, o) - outer * outer;
    const double disc = b * b - c;
    if (disc < 0.0) return false;
    double t = std::max(0.0, -b - std::sqrt(disc));
    const double t_end = -b + std::sqrt(disc);
    const double step = 0.25 * params.tile_size;
    glm::ivec2 tile;
    double prev = t;
    for (; t <= t_end; t += step) {
        const glm::dvec3 p = o + v * t;
        if (glm::length(p) <= top(p, tile)) {
            double lo = prev, hi = t;
            for (int i = 0; i < 20; ++i) {
                const double mid = 0.5 * (lo + hi);
                const glm::dvec3 q = o + v * mid;
                glm::ivec2 tq;
                if (glm::length(q) <= top(q, tq)) hi = mid; else lo = mid;
            }
            const glm::dvec3 at = o + v * hi;
            top(at, hit.tile);
            hit.point = glm::vec3(at);
            hit.normal = glm::vec3(glm::normalize(at));
            hit.t = static_cast<float>(hi);
            return true;
        }
        prev = t;
    }
    return false;
}

} // namespace


TerrainComponent::TerrainComponent() : logger_("terrain") {}

TerrainComponent::~TerrainComponent() {
    // Before anything else: TerrainSystem must never read `this` again once it is freed (see
    // forget_terrain()'s doc comment for why a destroyed terrain is not the same as an inactive
    // one to it).
    if (streaming_system_ != nullptr) streaming_system_->forget_terrain(this);
    if (jobs_ == nullptr) return;
    for (auto& entry : chunks_) {
        coopa::job::JobHandle& handle = entry.second.job;
        if (!handle.is_valid()) continue;
        jobs_->cancel(handle);   // stops whatever has not started
        jobs_->wait_for(handle); // ...and blocks on whatever has
        handle.close();
    }
}

const std::vector<TerrainLayer>& TerrainComponent::effective_layers() const {
    if (!layers.empty()) return layers;
    // Built once: the list is constant, and a scatter resolves a name every frame.
    static const std::vector<TerrainLayer> k_builtin = builtin_tile_layers();
    return k_builtin;
}

int TerrainComponent::layer_index(const std::string& name) const {
    const std::vector<TerrainLayer>& list = effective_layers();
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

glm::vec3 TerrainComponent::origin() const {
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) return glm::vec3(tc->transform().get_world_matrix()[3]);
    }
    return glm::vec3(0.0f);
}

void TerrainComponent::sync_field() {
    TerraGeometry g;
    if (mode == TerrainMode::Heightmap) {
        const int sub = std::max(1, height_sub);
        g.tiles_x = g.tiles_y = std::max(1, (heightmap_resolution - 1) / sub);
        g.tile_size = g.tiles_x > 0 ? heightmap_size.x / static_cast<float>(g.tiles_x) : 1.0f;
        g.height_sub = sub;
        g.layer_sub = g.tiles_x > 0 ? std::max(1, heightmap_splat_resolution / g.tiles_x) : 1;
        g.height = heightmap_height;
        g.height_step = params.height_step;
        g.chunk_tiles = g.tile_size > 0.0f
                            ? std::max(1, static_cast<int>(std::lround(heightmap_chunk_size / g.tile_size)))
                            : 16;
    } else {
        // mapcoopa decides the tile count at bake time; 0 tells sync below and TerraStore to keep
        // whatever is already loaded, rather than reshaping a baked world down to nothing.
        g.tiles_x = 0;
        g.tiles_y = 0;
        g.tile_size = params.tile_size;
        g.height_sub = 1;
        g.layer_sub = 1;
        g.height = params.height_scale;
        g.height_step = params.height_step;
        g.chunk_tiles = params.chunk_size;
    }
    g = g.sanitized();

    const TerraGeometry& cur = field.geometry();
    const bool size_matches = mode == TerrainMode::Heightmap ? (cur.tiles_x == g.tiles_x && cur.tiles_y == g.tiles_y) : true;
    if (size_matches && cur.tile_size == g.tile_size && cur.height_sub == g.height_sub &&
        cur.layer_sub == g.layer_sub && cur.height_step == g.height_step && cur.height == g.height &&
        cur.chunk_tiles == g.chunk_tiles) {
        return;
    }
    field.attach(TerraStore::instance().acquire(data_file, g));

    // A one-shot import, exactly as privileged as a mapcoopa bake and no more: only fires on a
    // field that has never been baked/imported/sculpted (needs_bake()), so a later re-parse (the
    // editor does one on every change) never clobbers whatever the field now holds.
    if (TerraData* d = field.data(); d != nullptr && d->needs_bake() && !import_height_path.empty()) {
        if (import_height_png(import_height_path, *d)) {
            if (!import_layer0_path.empty()) import_layer_png(import_layer0_path, *d, 0);
            if (!import_layer1_path.empty()) import_layer_png(import_layer1_path, *d, 1);
            d->clear_needs_bake();
            d->mark_all_dirty();
        } else {
            std::cerr << "[Terrain] '" << (owner != nullptr ? owner->name() : std::string())
                      << "': import_height_png('" << import_height_path << "') failed -- check the "
                         "path and that the terrain has a height plane (height_sub > 1)\n";
        }
    }
}

TerrainFieldView TerrainComponent::field_view() const {
    glm::vec3 o(0.0f);
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) o = glm::vec3(tc->transform().get_world_matrix()[3]);
    }
    return TerrainFieldView{&field, o};
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

bool TerrainComponent::tile_at(const glm::vec3& world, glm::ivec2& tile) const {
    if (field.empty()) return false;
    const glm::vec3 local = to_local_(world);
    if (params.planet) {
        // A planet's tiles are not laid out under x and y: they are six faces on a sphere, and
        // which one a point is over is the direction out of the centre (planet.h).
        if (glm::length(local) < 1e-4f) return false;
        tile = params.layout().tile_of(glm::normalize(glm::dvec3(local)));
        return field.contains(tile);
    }
    tile = field.tile_at(glm::vec2(local));
    return field.contains(tile);
}

glm::vec3 TerrainComponent::to_local_(const glm::vec3& world) const {
    if (owner == nullptr) return world;
    auto* tc = owner->get_transform();
    if (tc == nullptr) return world;
    return glm::vec3(glm::inverse(tc->transform().get_world_matrix()) * glm::vec4(world, 1.0f));
}

IRect TerrainComponent::raise_tiles(const IRect& rect, int delta) {
    if (field.empty() || delta == 0) return IRect{};
    const PlanetLayout layout = params.layout();
    const PlanetLayout* planet = params.planet ? &layout : nullptr;
    // settle = false: the ground moves now, the water is queued -- see the header.
    const IRect changed = tile_raise(field, rect, delta, false, planet);
    queue_settle_(changed);
    return changed;
}

IRect TerrainComponent::set_tile_height(const IRect& rect, int steps) {
    if (field.empty()) return IRect{};
    const PlanetLayout layout = params.layout();
    const PlanetLayout* planet = params.planet ? &layout : nullptr;
    const IRect changed = tile_set_height(field, rect, steps, false, planet);
    queue_settle_(changed);
    return changed;
}

IRect TerrainComponent::paint_tiles(const IRect& rect, TileKind kind, bool side) {
    if (field.empty()) return IRect{};
    return tile_paint(field, rect, kind, side);   // paint moves no ground: no water to settle
}

void TerrainComponent::queue_settle_(const IRect& changed) {
    if (changed.empty() || field.empty()) return;
    const PlanetLayout layout = params.layout();
    sync_field();
    water_settles.push_back(begin_settle(field, changed, IRect{}, params.planet ? &layout : nullptr));
}

bool TerrainComponent::raise_at(const glm::vec3& world, int radius_tiles, int delta) {
    glm::ivec2 tile;
    if (!tile_at(world, tile)) return false;
    const int r = std::max(0, radius_tiles);
    IRect rect{tile.x - r, tile.y - r, tile.x + r + 1, tile.y + r + 1};
    // On a planet a brush straddling a face's edge would run off into another face's columns,
    // which are nowhere near it in storage; clip it to the face the hit is on.
    if (params.planet) {
        const int n = std::max(1, params.face_tiles);
        const int face = std::clamp(tile.x / n, 0, 5);
        rect = IRect{std::max(rect.x0, face * n), std::max(rect.y0, 0),
                     std::min(rect.x1, (face + 1) * n), std::min(rect.y1, n)};
    }
    return !raise_tiles(rect, delta).empty();
}

bool TerrainComponent::tile_outline(const IRect& rect, std::array<glm::vec3, 4>& corners, float lift) const {
    if (field.empty()) return false;
    const int tiles_x = field.geometry().tiles_x, tiles_y = field.geometry().tiles_y;
    const IRect r = rect.clipped(tiles_x, tiles_y);
    if (r.empty()) return false;
    const float top = field.ground_at_tile(glm::ivec2((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2)) + lift;
    glm::mat4 to_world(1.0f);
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) to_world = tc->transform().get_world_matrix();
    }
    if (params.planet) {
        // Four points on the globe, in the range's own face: a flat quad would cut under the
        // surface at every corner.
        const PlanetLayout layout = params.layout();
        const std::int32_t n = std::max(1, params.face_tiles);
        const std::int32_t face = std::clamp(r.x0 / n, 0, 5);
        const double x0 = static_cast<double>(r.x0 - face * n) * params.tile_size;
        const double x1 = static_cast<double>(r.x1 - face * n) * params.tile_size;
        const double y0 = static_cast<double>(r.y0) * params.tile_size;
        const double y1 = static_cast<double>(r.y1) * params.tile_size;
        const double px[4] = {x0, x1, x1, x0};
        const double py[4] = {y0, y0, y1, y1};
        for (int i = 0; i < 4; ++i) {
            const glm::dvec3 p = layout.to_planet(face, px[i], py[i], top);
            corners[static_cast<std::size_t>(i)] = glm::vec3(to_world * glm::vec4(glm::vec3(p), 1.0f));
        }
        return true;
    }
    const float s = params.tile_size;
    const float x0 = static_cast<float>(r.x0) * s, x1 = static_cast<float>(r.x1) * s;
    const float y0 = static_cast<float>(r.y0) * s, y1 = static_cast<float>(r.y1) * s;
    const glm::vec3 flat[4] = {{x0, y0, top}, {x1, y0, top}, {x1, y1, top}, {x0, y1, top}};
    for (int i = 0; i < 4; ++i) {
        corners[static_cast<std::size_t>(i)] = glm::vec3(to_world * glm::vec4(flat[i], 1.0f));
    }
    return true;
}

bool TerrainComponent::raycast_tiles(const glm::vec3& origin, const glm::vec3& dir, float max_t,
                                     TileHit& hit) const {
    const TerraData* d = field.data();
    if (!d || d->empty()) return false;
    const glm::vec3 o = to_local_(origin);
    glm::vec3 d2 = dir;
    if (owner != nullptr) {
        if (auto* tc = owner->get_transform()) {
            d2 = glm::vec3(glm::inverse(tc->transform().get_world_matrix()) * glm::vec4(dir, 0.0f));
        }
    }
    if (glm::length(d2) < 1e-6f) return false;
    d2 = glm::normalize(d2);
    if (params.planet) return planet_raycast(*d, params, o, d2, hit);
    return d->raycast_columns(o, d2, max_t, hit);
}

const TileCell* TerrainComponent::record_at(const glm::vec3& world) const {
    glm::ivec2 tile;
    return tile_at(world, tile) ? field.record_at(tile) : nullptr;
}

float TerrainComponent::height_at(const glm::vec3& world) const {
    glm::ivec2 tile;
    float z = tile_at(world, tile) ? field.ground_at_tile(tile) : 0.0f;
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
