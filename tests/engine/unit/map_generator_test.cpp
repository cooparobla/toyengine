/**
 * @file map_generator_test.cpp
 * @brief Generating a world and applying it to a terrain, device-free -- in both terrain modes.
 *
 * Map generation is a component of its own (toyengine/mapgen), and what it does to a terrain is
 * *apply*: it writes the terrain's own data and then steps out of the way. These tests hold that
 * contract from both ends -- that a tile world comes out terraced with its sea, lakes and rivers
 * where the map says, and that the SAME map written to a smooth terrain gives it relief, layers
 * and a water surface it could never have had before.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <map>
#include <string>

#include <coopa/debug/logger.h>
#include <glm/glm.hpp>

#include <toyengine/mapgen/map_generator_component.h>
#include <toyengine/world/heightmap_data.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("map_generator");

using toy::mapgen::AutoApply;
using toy::mapgen::MapGeneratorComponent;
using toy::world::TerrainComponent;
using toy::world::TerrainFieldGeometry;
using toy::world::TerrainMode;
using toy::world::TileRecord;
using toy::world::WaterKind;

namespace {

// Both components own unique_ptrs and so are not copyable: these configure one in place rather
// than handing one back.

/**
 * @brief A small, fast, deterministic world -- the shape terrain_sampler_test generates.
 *
 * `grid_size` is 40 rather than something smaller because mapcoopa needs a certain amount of
 * land before its river pass has anywhere to run: below about 32 cells it reports "too little
 * land in the source elevation band" and places none, and a world with no rivers cannot show
 * whether an apply carries them.
 */
void generate(MapGeneratorComponent& g, std::uint32_t seed = 7) {
    g.config.seed = seed;
    g.config.grid_size = 40;
    g.world_tiles = 160;         // four tiles per cell: enough for coastlines and a river channel
    g.height_scale = 32.0f;
    g.begin(nullptr);            // no job engine: generates here and now
}

/** @brief A tiles-mode terrain with nothing in it yet. */
void make_tile_terrain(TerrainComponent& t) {
    t.mode = TerrainMode::Tiles;
    t.params.world_tiles = 160;
    t.params.tile_size = 1.0f;
    t.params.height_step = 1.0f;
    t.params.height_scale = 32.0f;
    t.params.chunk_size = 16;
    t.tiles = std::make_shared<toy::world::TileMapData>();
}

/** @brief A heightmap-mode terrain with a flat plane and no paint. */
void make_smooth_terrain(TerrainComponent& t) {
    t.mode = TerrainMode::Heightmap;
    toy::world::HeightmapGeometry g;
    g.size = glm::vec2(160.0f);
    g.height = 32.0f;
    g.chunk_size = 16.0f;
    g.resolution = 161;          // one sample a metre, so 160 tiles a side
    g.splat_resolution = 160;
    t.data = std::make_shared<toy::world::HeightmapData>(g.sanitized());
}

/** @brief Runs an apply to completion (synchronous, with no job engine). */
bool apply(MapGeneratorComponent& g, TerrainComponent& t) {
    if (!g.apply_to(t, nullptr)) return false;
    return g.poll_apply();
}

} // namespace

/** @brief A world generates without a job engine, and is queryable once it has. */
COOPA_TEST(a_world_generates_and_is_queryable) {
    MapGeneratorComponent g;
    generate(g);
    require(g.ready(), "mapgen: the world generated");
    require(g.graph() != nullptr, "mapgen: ...and the graph is there to read");
    expect(!g.graph()->centers.empty(), "mapgen: with cells in it");
    expect(g.generated_config().grid_size == g.config.grid_size,
           "mapgen: generated at the size that was asked for");
    expect(!g.generating() && !g.applying(), "mapgen: and nothing is left in flight");
}

/** @brief Applied to a tile terrain, the map becomes terraced ground with its water on it. */
COOPA_TEST(a_map_applies_to_a_tile_terrain) {
    MapGeneratorComponent g;
    generate(g);
    TerrainComponent t;
    make_tile_terrain(t);
    require(apply(g, t), "tiles: the apply ran");

    require(t.tiles && !t.tiles->empty(), "tiles: the grid is filled");
    expect(t.tiles->width() == 160 && t.tiles->height() == 160, "tiles: at the size the generator asked for");
    expect(t.applied_map != nullptr, "tiles: the terrain keeps the map it was given");

    int wet = 0, dry = 0, ocean = 0, river = 0;
    std::int16_t lowest = 32767, highest = -32768;
    std::map<WaterKind, int> kinds;
    for (int y = 0; y < t.tiles->height(); ++y) {
        for (int x = 0; x < t.tiles->width(); ++x) {
            const TileRecord& r = t.tiles->at(x, y);
            lowest = std::min(lowest, r.steps);
            highest = std::max(highest, r.steps);
            if (r.water()) {
                ++wet;
                if (r.has(TileRecord::Ocean)) ++ocean;
                if (r.has(TileRecord::Flowing)) ++river;
            } else {
                ++dry;
            }
            ++kinds[t.field.water_at(glm::ivec2(x, y)).kind];
        }
    }
    expect(wet > 0 && dry > 0, "tiles: the world has both land and water");
    expect(highest > lowest, "tiles: ...and relief between them");
    expect(ocean > 0, "tiles: the sea is marked as the sea");
    expect(river > 0, "tiles: and a river is marked as flowing (" + std::to_string(river) + " tiles)");
    int with_flow = 0;
    for (int y = 0; y < t.tiles->height(); ++y) {
        for (int x = 0; x < t.tiles->width(); ++x) {
            const TileRecord& r = t.tiles->at(x, y);
            if (r.has(TileRecord::Flowing) && r.flow >= 1 && r.flow <= 8) ++with_flow;
        }
    }
    expect(with_flow == river, "tiles: every river tile carries a downstream direction");

    // The same water, read through the unified field: three kinds, not one flag soup.
    expect(kinds[WaterKind::Ocean] == ocean, "tiles: the field reports the sea as Ocean");
    expect(kinds[WaterKind::River] == river, "tiles: ...and a river as River");
    expect(kinds[WaterKind::None] == dry, "tiles: ...and dry ground as None");
}

/**
 * @brief The same map applied to a SMOOTH terrain: relief in the height plane, layers under it,
 *        and a water surface a heightmap could not carry before.
 */
COOPA_TEST(the_same_map_applies_to_a_heightmap_terrain) {
    MapGeneratorComponent g;
    generate(g);
    TerrainComponent t;
    make_smooth_terrain(t);
    require(apply(g, t), "heightmap: the apply ran");
    require(t.data != nullptr, "heightmap: the plane is there");

    float lowest = 1e9f, highest = -1e9f;
    for (const float h : t.data->heights()) {
        lowest = std::min(lowest, h);
        highest = std::max(highest, h);
    }
    expect(highest - lowest > 1.0f,
           "heightmap: the plane has relief (" + std::to_string(highest - lowest) + " m)");
    expect(highest <= 32.0f + 1e-3f, "heightmap: ...within the terrain's height");

    // Heights are continuous, not snapped to a step: a terraced bake would give a handful of
    // distinct values over a whole world, and this is the difference the smooth path exists for.
    std::map<int, int> millimetre_buckets;
    for (const float h : t.data->heights()) ++millimetre_buckets[static_cast<int>(h * 1000.0f)];
    expect(millimetre_buckets.size() > 500,
           "heightmap: " + std::to_string(millimetre_buckets.size()) + " distinct heights, so not terraced");

    // Layers: the generated surface is painted, not left blank.
    int painted = 0;
    for (int j = 0; j < t.data->splat_h(); ++j) {
        for (int i = 0; i < t.data->splat_w(); ++i) {
            int total = 0;
            for (int l = 0; l < toy::world::k_heightmap_layer_count; ++l) total += t.data->splat_at(l, i, j);
            if (total > 0) ++painted;
        }
    }
    expect(painted > 0, "heightmap: the surface is painted from the map's own biomes");

    // Water: the field carries a level per tile, and the ground under a wet tile is below it.
    int wet = 0, below = 0;
    std::map<WaterKind, int> kinds;
    for (int ty = 0; ty < t.field.geometry().tiles_y; ++ty) {
        for (int tx = 0; tx < t.field.geometry().tiles_x; ++tx) {
            const auto w = t.field.water_at(glm::ivec2(tx, ty));
            ++kinds[w.kind];
            if (!w.wet()) continue;
            ++wet;
            if (t.field.ground_at_tile(glm::ivec2(tx, ty)) < w.level) ++below;
        }
    }
    expect(wet > 0, "heightmap: a smooth terrain now carries water (" + std::to_string(wet) + " tiles)");
    expect(below == wet, "heightmap: and every wet tile's ground is under its own surface");
    expect(kinds[WaterKind::Ocean] > 0, "heightmap: the sea came across as the sea");
    expect(t.applied_map != nullptr, "heightmap: the terrain keeps the map it was given");
}

/** @brief One seed is one world: the same settings apply identically twice. */
COOPA_TEST(a_seed_is_a_world) {
    MapGeneratorComponent a, b, c;
    generate(a, 11);
    generate(b, 11);
    generate(c, 12);
    TerrainComponent ta, tb, tc;
    make_tile_terrain(ta);
    make_tile_terrain(tb);
    make_tile_terrain(tc);
    require(apply(a, ta) && apply(b, tb) && apply(c, tc), "seed: all three applied");

    bool same = true, differs = false;
    for (int y = 0; y < ta.tiles->height() && same; ++y) {
        for (int x = 0; x < ta.tiles->width(); ++x) {
            if (ta.tiles->at(x, y).steps != tb.tiles->at(x, y).steps) { same = false; break; }
            if (ta.tiles->at(x, y).steps != tc.tiles->at(x, y).steps) differs = true;
        }
    }
    expect(same, "seed: the same seed bakes the same ground");
    expect(differs, "seed: a different seed bakes different ground");
}

/** @brief `auto_apply:` round-trips through its YAML spelling. */
COOPA_TEST(auto_apply_names_round_trip) {
    using toy::mapgen::auto_apply_from_name;
    using toy::mapgen::auto_apply_name;
    for (const AutoApply mode : {AutoApply::WhenMissing, AutoApply::Never, AutoApply::Always}) {
        expect(auto_apply_from_name(auto_apply_name(mode)) == mode,
               std::string("auto_apply: `") + auto_apply_name(mode) + "` round-trips");
    }
    expect(auto_apply_from_name("nonsense") == AutoApply::WhenMissing,
           "auto_apply: an unknown name falls back to when_missing");
}
