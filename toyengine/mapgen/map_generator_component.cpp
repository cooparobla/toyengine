#include <toyengine/mapgen/map_generator_component.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include <coopa/maps/map_presets.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/core/yaml_read.h>
#include <toyengine/mapgen/terrain_generator.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/terrain_water.h>

namespace toy {
namespace mapgen {

namespace {

/// Job types, for the profiler's per-job breakdown.
constexpr std::uint32_t k_apply_job_type = 0x7E44A101u;

} // namespace

AutoApply auto_apply_from_name(const std::string& name) {
    if (name == "never") return AutoApply::Never;
    if (name == "always") return AutoApply::Always;
    return AutoApply::WhenMissing;
}

const char* auto_apply_name(AutoApply mode) {
    switch (mode) {
        case AutoApply::Never:  return "never";
        case AutoApply::Always: return "always";
        case AutoApply::WhenMissing: break;
    }
    return "when_missing";
}

/**
 * @struct MapGeneratorComponent::Bake
 * @brief One apply in flight.
 *
 * A whole grid rather than a patch: a bake writes every tile, and the target only takes it once
 * the worker has finished, so a half-written world is never visible.
 */
struct MapGeneratorComponent::Bake {
    world::TileMapData tiles;            ///< The Tiles target's new grid.
    std::vector<float> heights;          ///< The Heightmap target's new plane.
    std::vector<std::uint8_t> splat0, splat1;
    std::vector<world::WaterCell> water; ///< Per tile, both modes.
    int water_w = 0, water_h = 0;
    bool heightmap = false;
};

MapGeneratorComponent::MapGeneratorComponent() : logger_("mapgen"), config(default_generator()) {}
MapGeneratorComponent::~MapGeneratorComponent() {
    // An apply in flight holds a handle, and every handle must be closed exactly once. The job
    // itself only touches shared_ptrs it captured, so it is safe for it to finish on its own --
    // what is not safe is dropping the handle unclosed.
    if (bake_handle_.is_valid() && jobs_ != nullptr) {
        jobs_->cancel(bake_handle_);
        jobs_->wait_for(bake_handle_);
    }
    if (bake_handle_.is_valid()) bake_handle_.close();
}

void MapGeneratorComponent::begin(coopa::job::JobEngine* jobs) {
    jobs_ = jobs;
    if (generator_ != nullptr || ready()) return;

    // The values the CLI derives after reading a config (noise seeds from the master seed, the
    // island field scaled to the grid) -- without them one seed would make the same coastline as
    // another. A globe is the same settings generated on a sphere.
    coopa::maps::MapConfig authored = config;
    if (sphere) authored.topology = coopa::maps::MapTopology::Sphere;
    generated_config_ = coopa::maps::derived_config(authored);

    generator_ = std::make_unique<coopa::maps::MapGenerator>(generated_config_, logger_);
    if (jobs == nullptr) {
        // No job engine: generate here and now. A scene always has one, so this is the headless
        // and unit-test path -- and the same fallback apply_to() takes for the bake.
        generator_->generate();
        graph_ = std::make_unique<coopa::maps::MapGraph>(std::move(generator_->graph()));
        generator_.reset();
        return;
    }
    generator_->set_job_engine(jobs);
    // The generator must outlive the task -- the job writes into its graph -- and it does: both
    // are members, and task_ is declared after generator_ so it destructs first.
    task_ = generator_->generate_async();
}

bool MapGeneratorComponent::poll() {
    if (generator_ == nullptr || !task_.done()) return false;
    graph_ = std::make_unique<coopa::maps::MapGraph>(std::move(generator_->graph()));
    task_ = coopa::maps::MapTask{};
    generator_.reset();
    return graph_ != nullptr;
}

world::TerrainParams MapGeneratorComponent::bake_params_(const world::TerrainComponent& terrain) const {
    // The target's own tiling, with this generator's keys over the parts that only a bake uses.
    world::TerrainParams p = terrain.params;
    p.world_tiles = world_tiles;
    p.tiles_per_grid_unit = tiles_per_grid_unit;
    p.height_scale = height_scale;
    p.water_inset = water_inset;
    p.river_inset = river_inset;
    return p;
}

bool MapGeneratorComponent::apply_to(world::TerrainComponent& terrain, coopa::job::JobEngine* jobs) {
    if (!ready() || applying()) return false;
    jobs_ = jobs != nullptr ? jobs : jobs_;
    target_ = &terrain;

    auto bake = std::make_shared<Bake>();
    bake->heightmap = terrain.mode == world::TerrainMode::Heightmap;

    // Built here, not when the graph landed: reading the map per tile needs the target's own
    // quantisation and layout. A rebuild is only the site index -- cheap beside the bake.
    auto built = std::make_shared<world::TerrainSampler>();
    built->build(generated_config_, coopa::maps::MapGraph(*graph_), bake_params_(terrain));
    if (!built->is_built()) return false;
    std::shared_ptr<const world::TerrainSampler> sampler = built;
    sampler_ = sampler;

    if (!bake->heightmap) {
        // The terraced world: exactly the bake a tile terrain has always had.
        auto run = [bake, sampler]() { sampler->bake(bake->tiles); };
        bake_ = bake;
        if (jobs_ != nullptr) {
            bake_handle_ = jobs_->create_handle();
            jobs_->submit(run, k_apply_job_type, bake_handle_, nullptr, 0u, coopa::job::Priority::Normal);
        } else {
            run();
        }
        return true;
    }

    // The smooth world: the height plane, the layer weights under it, and the water over it.
    if (!terrain.data) return false;
    const world::HeightmapGeometry g = terrain.data->geometry();
    const int nx = g.resolution, ny = g.resolution;
    const int sw = g.splat_resolution, sh = g.splat_resolution;
    const int tiles_x = std::max(1, nx - 1), tiles_y = std::max(1, ny - 1);
    bake->heights.assign(static_cast<std::size_t>(nx) * ny, 0.0f);
    bake->splat0.assign(static_cast<std::size_t>(sw) * sh * 4, 0);
    bake->splat1.assign(static_cast<std::size_t>(sw) * sh * 4, 0);
    bake->water.assign(static_cast<std::size_t>(tiles_x) * tiles_y, world::WaterCell{});
    bake->water_w = tiles_x;
    bake->water_h = tiles_y;

    // The map names a surface as a TileKind; the terrain names its own in `layers:`. Resolve
    // one to the other BY NAME, not by index -- a terrain's list is its own, and only the
    // built-in set happens to be in TileKind order. A kind the terrain has no layer for falls
    // back to its first, which is the one a heightmap terrain always has.
    std::array<int, world::k_tile_kind_count> layer_for_kind{};
    for (std::size_t k = 0; k < world::k_tile_kind_count; ++k) {
        layer_for_kind[k] = 0;
        const char* name = world::tile_kind_names()[k];
        for (std::size_t i = 0; i < terrain.layers.size() && i < world::k_heightmap_layer_count; ++i) {
            if (terrain.layers[i].name == name) { layer_for_kind[k] = static_cast<int>(i); break; }
        }
    }

    // The map covers the terrain's full extent, whatever resolution each is at: a sample at
    // normalised (u, v) reads the map at that fraction of its tile grid.
    const double map_tiles = static_cast<double>(sampler->params().tiles_for_grid(generated_config_.grid_size));
    auto run = [bake, sampler, nx, ny, sw, sh, tiles_x, tiles_y, map_tiles, layer_for_kind]() {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const double u = nx > 1 ? static_cast<double>(i) / (nx - 1) : 0.0;
                const double v = ny > 1 ? static_cast<double>(j) / (ny - 1) : 0.0;
                const auto s = sampler->sample_surface(u * map_tiles, v * map_tiles);
                bake->heights[static_cast<std::size_t>(j) * nx + i] = s.ground;
            }
        }
        // Layers at the splat plane's own resolution: one dominant surface per texel, which is
        // what a generated world has to say. Painting over it afterwards blends as usual.
        for (int j = 0; j < sh; ++j) {
            for (int i = 0; i < sw; ++i) {
                const double u = (static_cast<double>(i) + 0.5) / sw;
                const double v = (static_cast<double>(j) + 0.5) / sh;
                const auto s = sampler->sample_surface(u * map_tiles, v * map_tiles);
                const std::size_t kind = static_cast<std::size_t>(s.top);
                const int layer = kind < layer_for_kind.size() ? layer_for_kind[kind] : 0;
                auto& plane = layer < 4 ? bake->splat0 : bake->splat1;
                plane[(static_cast<std::size_t>(j) * sw + i) * 4 + (layer % 4)] = 255;
            }
        }
        // Water per tile, as a surface level: the shoreline is wherever the ground crosses it.
        for (int ty = 0; ty < tiles_y; ++ty) {
            for (int tx = 0; tx < tiles_x; ++tx) {
                const double u = (static_cast<double>(tx) + 0.5) / tiles_x;
                const double v = (static_cast<double>(ty) + 0.5) / tiles_y;
                const auto s = sampler->sample_surface(u * map_tiles, v * map_tiles);
                world::WaterCell c;
                switch (s.water) {
                    case coopa::maps::WaterKind::Sea:   c.kind = world::WaterKind::Ocean; break;
                    case coopa::maps::WaterKind::Lake:  c.kind = world::WaterKind::Lake; break;
                    case coopa::maps::WaterKind::River: c.kind = world::WaterKind::River; break;
                    default: break;
                }
                c.level = s.water_surface;
                c.flow = s.flow;
                bake->water[static_cast<std::size_t>(ty) * tiles_x + tx] = c;
            }
        }
    };
    bake_ = bake;
    if (jobs_ != nullptr) {
        bake_handle_ = jobs_->create_handle();
        jobs_->submit(run, k_apply_job_type, bake_handle_, nullptr, 0u, coopa::job::Priority::Normal);
    } else {
        run();
    }
    return true;
}

bool MapGeneratorComponent::poll_apply() {
    if (bake_ == nullptr) return false;
    if (bake_handle_.is_valid() && !bake_handle_.is_complete()) return false;
    if (bake_handle_.is_valid()) {
        bake_handle_.close();
        bake_handle_ = coopa::job::JobHandle{};
    }
    std::shared_ptr<Bake> bake = std::move(bake_);
    bake_.reset();
    world::TerrainComponent* terrain = target_;
    if (terrain == nullptr) return false;

    if (!bake->heightmap) {
        if (terrain->tiles) terrain->tiles->assign_baked(std::move(bake->tiles));
        terrain->applied_map = sampler_;
        terrain->sync_field();
        return true;
    }

    if (!terrain->data) return false;
    terrain->data->heights() = std::move(bake->heights);
    terrain->data->splat(0) = std::move(bake->splat0);
    terrain->data->splat(1) = std::move(bake->splat1);
    terrain->data->mark_all_dirty();
    terrain->sync_field();
    for (int ty = 0; ty < bake->water_h; ++ty) {
        for (int tx = 0; tx < bake->water_w; ++tx) {
            terrain->field.set_water(glm::ivec2(tx, ty),
                                     bake->water[static_cast<std::size_t>(ty) * bake->water_w + tx]);
        }
    }
    terrain->field.mark_all_dirty();
    terrain->applied_map = sampler_;
    return true;
}

MapGeneratorComponent* generator_for(coopa::scene::Scene& scene, const world::TerrainComponent& terrain) {
    MapGeneratorComponent* fallback = nullptr;
    for (MapGeneratorComponent* g : scene.get_components<MapGeneratorComponent>()) {
        if (g->owner == nullptr || !g->owner->active()) continue;
        if (!g->target.empty()) {
            if (terrain.owner != nullptr && terrain.owner->name() == g->target) return g;
            continue;
        }
        // No target named: the terrain on this generator's own object, else the only one going.
        if (terrain.owner != nullptr && g->owner == terrain.owner) return g;
        if (fallback == nullptr) fallback = g;
    }
    return fallback;
}

namespace {

/**
 * @class MapGeneratorSystem
 * @brief Drives every generator: generate once, then apply when its target asks for it.
 *
 * At order 59, just before TerrainSystem: a world that lands this frame is already in the tiles
 * by the time the streamer decides what to mesh, so the first chunks are never built from an
 * empty grid.
 */
class MapGeneratorSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "MapGenerator"; }
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override {
        adopt_legacy_terrains_(scene);
        const auto generators = scene.get_components<MapGeneratorComponent>();
        if (generators.empty()) return;
        const auto terrains = scene.get_components<world::TerrainComponent>();

        for (MapGeneratorComponent* g : generators) {
            if (g->owner == nullptr) continue;
            // A generator with nothing to give and nobody asking stays idle: generation is the
            // expensive half, so it waits until a target actually wants a world.
            world::TerrainComponent* target = nullptr;
            for (world::TerrainComponent* t : terrains) {
                if (generator_for(scene, *t) != g) continue;
                if (wants_world_(*g, *t)) { target = t; break; }
            }
            if (target == nullptr && !g->applying()) continue;

            // The terrain reports "being generated" itself, so the editor's Regenerating... and
            // the frame profile's warm-up gate need know nothing about generators.
            world::TerrainComponent* busy = target != nullptr ? target : g->applied_target();
            if (busy != nullptr) busy->baking_ = true;

            g->begin(ctx.jobs);
            g->poll();
            const bool landed = g->poll_apply();
            if (!landed && target != nullptr && g->ready() && !g->applying()) {
                g->apply_to(*target, ctx.jobs);
            }
            if (busy != nullptr && !g->generating() && !g->applying()) busy->baking_ = false;
        }
    }

private:
    /**
     * @brief Gives a tiles terrain that has no generator one made from its own `generator:` block.
     *
     * Every tile scene written so far puts that block on its terrain, and a bare `TileTerrain`
     * with none at all has always meant mapcoopa's standard map -- so without this they would
     * stop generating. Done here rather than in the parser because the parser sees one component
     * at a time: a scene that DOES author a MapGenerator lists it after its terrain, and the
     * parser would synthesise a second one before reaching it.
     *
     * The components are added after the walk, never during it, because adding one invalidates
     * the list being walked.
     */
    void adopt_legacy_terrains_(coopa::scene::Scene& scene) {
        std::vector<world::TerrainComponent*> orphans;
        for (world::TerrainComponent* t : scene.get_components<world::TerrainComponent>()) {
            if (t->mode != world::TerrainMode::Tiles || t->owner == nullptr) continue;
            if (generator_for(scene, *t) != nullptr) continue;
            orphans.push_back(t);
        }
        for (world::TerrainComponent* t : orphans) {
            auto* g = t->owner->add_component<MapGeneratorComponent>();
            g->config = t->generator;
            g->sphere = t->params.planet;
            g->world_tiles = t->params.world_tiles;
            g->tiles_per_grid_unit = t->params.tiles_per_grid_unit;
            g->height_scale = t->params.height_scale;
            g->water_inset = t->params.water_inset;
            g->river_inset = t->params.river_inset;
            g->auto_apply = AutoApply::WhenMissing;
        }
    }

    /** @brief Whether `terrain` is asking this generator for a world right now. */
    static bool wants_world_(const MapGeneratorComponent& g, const world::TerrainComponent& terrain) {
        if (g.auto_apply == AutoApply::Never) return false;
        if (g.auto_apply == AutoApply::Always) return terrain.applied_map == nullptr;
        // WhenMissing: the target has no data of its own. A tile world says so itself (its
        // `.tiles` file was absent, or the editor asked to regenerate); a heightmap's PNG is
        // generated by its own store, so it is never missing by the time we get here.
        if (terrain.mode == world::TerrainMode::Tiles) {
            return terrain.tiles != nullptr && terrain.tiles->needs_bake();
        }
        return false;
    }
};

} // namespace

void register_map_generator_component() {
    using coopa::scene::SceneLoader;
    SceneLoader::register_component_parser(
        "MapGenerator",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* g = obj.add_component<MapGeneratorComponent>();
            // Any mapcoopa key, over its standard map -- the same block a terrain's `generator:`
            // has always been, so a scene can move one across verbatim.
            g->config = generator_config_from_node(node);
            if (node.contains("target")) g->target = node.at("target").get_value<std::string>();
            if (node.contains("auto_apply")) {
                g->auto_apply = auto_apply_from_name(node.at("auto_apply").get_value<std::string>());
            }
            if (node.contains("sphere")) g->sphere = node.at("sphere").get_value<bool>();
            const auto num = [&node](const char* key, double fallback) {
                return node.contains(key) ? toy::core::yaml::num(node.at(key), fallback) : fallback;
            };
            g->world_tiles = static_cast<std::int32_t>(num("world_tiles", g->world_tiles));
            g->tiles_per_grid_unit = static_cast<std::int32_t>(num("tiles_per_grid_unit", g->tiles_per_grid_unit));
            g->height_scale = static_cast<float>(num("height_scale", g->height_scale));
            g->water_inset = static_cast<float>(num("water_inset", g->water_inset));
            g->river_inset = static_cast<float>(num("river_inset", g->river_inset));
        });
}

void install_map_generator_system(coopa::scene::Scene& scene, int order) {
    scene.add_system(std::make_unique<MapGeneratorSystem>(), order);
}

} // namespace mapgen
} // namespace toy
