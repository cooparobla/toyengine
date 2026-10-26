/**
 * @file map_generator_component.h
 * @brief The `MapGenerator` component: a generated world, and the act of applying it to a terrain.
 *
 * Map generation is not part of a terrain. A `MapGenerator` sits on its own object, owns a
 * [mapcoopa](../../libs/mapcoopa) `MapConfig` and the `MapGraph` it generates, and **applies**
 * that world to a terrain on demand -- the same gesture as importing a heightmap PNG, and with
 * the same result: the terrain's own data is written, and from then on the terrain is the
 * authority. Nothing about drawing, streaming or editing a terrain goes back through here.
 *
 * ### What applying means in each mode
 *
 * | Target | What is written |
 * |---|---|
 * | `TerrainMode::Tiles` | Ground in whole steps, surface layers, biome, climate, region / country ids, and the sea, lakes and rivers -- the bake (TerrainSampler::bake()). |
 * | `TerrainMode::Heightmap` | The float height plane, splat weights from the surface and biome, and the same water field over it. |
 *
 * The map is the same either way; only the resolution it is written at differs, which is what
 * makes "apply a generated world to a smooth terrain" possible at all.
 *
 * ### It is also a queryable world map
 *
 * A generated map holds much more than ground: nations, provinces, settlements with streets,
 * roads, landmarks and cave systems. Gameplay reads those through `graph()` -- which is why the
 * component outlives the apply rather than being a one-shot tool.
 */

#ifndef TOYENGINE_MAPGEN_MAP_GENERATOR_COMPONENT_H
#define TOYENGINE_MAPGEN_MAP_GENERATOR_COMPONENT_H

#include <cstdint>
#include <memory>
#include <string>

#include <coopa/debug/logger.h>
#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_task.h>
#include <coopa/scene/component.h>

#include <toyengine/mapgen/terrain_sampler.h>
#include <toyengine/world/terrain_params.h>

namespace coopa {
namespace scene {
class Scene;
} // namespace scene
} // namespace coopa

namespace toy {
namespace world {
class TerrainComponent;
}

namespace mapgen {

/** @brief When a generator applies itself without being asked to. */
enum class AutoApply : std::uint8_t {
    /**
     * @brief When the target has no data of its own yet -- the default, and what a scene wants.
     *
     * A terrain whose `.tiles` file or heightmap PNG is missing is generated on load; once the
     * file exists, the stored data wins and the generator only matters to the next explicit
     * apply. That is how a seeded world stays reproducible without re-baking every run.
     */
    WhenMissing,
    /** @brief Only an explicit apply (the editor's button, or a call). */
    Never,
    /** @brief Every load, discarding whatever the target had stored. */
    Always
};

/** @brief `auto_apply:` as scene YAML spells it; unknown text reads as WhenMissing. */
AutoApply auto_apply_from_name(const std::string& name);
/** @brief The YAML spelling. */
const char* auto_apply_name(AutoApply mode);

/**
 * @class MapGeneratorComponent
 * @brief See the file comment. Generation and the apply both run on the job engine.
 */
class MapGeneratorComponent : public coopa::scene::Component {
public:
    MapGeneratorComponent();
    ~MapGeneratorComponent() override;

    std::string type_name() const override { return "MapGenerator"; }

    // ------------------------------------------------------------------ authored

    /**
     * @brief The mapcoopa settings, as authored.
     *
     * `coopa::maps::derived_config()` is applied at generation time, the way the CLI does it, so
     * one seed moves the coastline rather than only the noise.
     */
    coopa::maps::MapConfig config;

    /** @brief The object carrying the terrain to apply to; empty = a terrain on this object. */
    std::string target;

    AutoApply auto_apply = AutoApply::WhenMissing;

    /** @brief Generate on a globe rather than a plane (a planet terrain sets it). */
    bool sphere = false;

    // --- How the map is written into a Tiles target (ignored by a Heightmap one) ---

    /** @brief Tiles per side the map is rasterised into; 0 derives it from the grid. */
    std::int32_t world_tiles = 0;
    /** @brief Tiles per mapcoopa grid cell, when `world_tiles` is 0. */
    std::int32_t tiles_per_grid_unit = 4;
    /** @brief World height of the full elevation range, in metres. */
    float height_scale = 16.0f;
    /** @brief Steps the sea and lakes sit under the terrace they fill. */
    float water_inset = 0.5f;
    /** @brief The same for rivers, nearer their bank tops. */
    float river_inset = 0.2f;

    // ------------------------------------------------------------------ generation

    /** @brief Starts generating if nothing is in flight and no graph exists yet. */
    void begin(coopa::job::JobEngine* jobs);

    /** @brief Advances generation; true on the frame the graph lands. */
    bool poll();

    /** @brief A generate is in flight. */
    bool generating() const { return generator_ != nullptr; }

    /** @brief A world exists and can be applied or queried. */
    bool ready() const { return graph_ != nullptr; }

    /** @brief The generated world, or null before it lands. */
    const coopa::maps::MapGraph* graph() const { return graph_.get(); }

    /** @brief The settings it was generated with (derived, not authored). */
    const coopa::maps::MapConfig& generated_config() const { return generated_config_; }

    /**
     * @brief The per-tile view of the world, shared with whatever it was last applied to.
     *
     * Built by an apply, not by generation: reading the map per tile needs the TARGET's
     * quantisation and layout (its `height_step`, and a planet's cube-sphere), which only the
     * apply knows. Null until then.
     */
    std::shared_ptr<const world::TerrainSampler> sampler() const { return sampler_; }

    // ------------------------------------------------------------------ applying

    /**
     * @brief Writes this world into `terrain`, in whichever mode the terrain is.
     *
     * Asynchronous: the heavy half (a million samples on a large world) runs on `jobs`, and
     * poll_apply() lands it. Starts nothing while a previous apply is still running.
     *
     * @return False when there is no world yet, or an apply is already in flight.
     */
    bool apply_to(world::TerrainComponent& terrain, coopa::job::JobEngine* jobs);

    /** @brief An apply is in flight. */
    bool applying() const { return bake_ != nullptr; }

    /** @brief Advances an apply; true on the frame it lands in the terrain. */
    bool poll_apply();

    /** @brief The terrain this generator last applied to, or null. */
    world::TerrainComponent* applied_target() const { return target_; }

private:
    /** @brief The tiling the Tiles bake writes at, from this generator's own keys. */
    world::TerrainParams bake_params_(const world::TerrainComponent& terrain) const;

    /** @brief One apply in flight: the grid the worker fills, and where it lands. */
    struct Bake;

    coopa::debug::Logger logger_;
    coopa::maps::MapConfig generated_config_;
    /** @brief Released once its graph has been moved out. */
    std::unique_ptr<coopa::maps::MapGenerator> generator_;
    /** @brief Declared AFTER generator_: the job writes into its graph, so it must destruct first. */
    coopa::maps::MapTask task_;
    /** @brief The generated world, held from the frame it lands -- the queryable half. */
    std::unique_ptr<coopa::maps::MapGraph> graph_;
    /** @brief Shared, because whatever this is applied to keeps reading the map through it. */
    std::shared_ptr<const world::TerrainSampler> sampler_;

    std::shared_ptr<Bake> bake_;
    coopa::job::JobHandle bake_handle_;
    world::TerrainComponent* target_ = nullptr;
    coopa::job::JobEngine* jobs_ = nullptr;
};

/** @brief The generator a terrain is driven by: its `target`, or one on the terrain's own object. */
MapGeneratorComponent* generator_for(coopa::scene::Scene& scene, const world::TerrainComponent& terrain);

/**
 * @brief Order of MapGeneratorSystem -- just before TerrainSystem (60), so a world that lands
 *        this frame is already in the tiles when the streamer decides what to mesh.
 */
inline constexpr int k_map_generator_system_order = 59;

/** @brief Installs the system that drives every generator's generate and auto-apply. */
void install_map_generator_system(coopa::scene::Scene& scene, int order = k_map_generator_system_order);

/** @brief Registers the "MapGenerator" scene-file parser. */
void register_map_generator_component();

} // namespace mapgen
} // namespace toy

#endif // TOYENGINE_MAPGEN_MAP_GENERATOR_COMPONENT_H
