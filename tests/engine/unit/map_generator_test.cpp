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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/passes/water_levels.h>
#include <coopa/yaml/document.h>
#include <glm/glm.hpp>

#include <toyengine/mapgen/map_generator_component.h>
#include <toyengine/mapgen/terrain_generator.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("map_generator");

using toy::mapgen::AutoApply;
using toy::mapgen::MapGeneratorComponent;
using toy::world::TerraData;
using toy::world::TerraGeometry;
using toy::world::TerrainComponent;
using toy::world::TerrainMode;
using toy::world::TileCell;
using toy::world::WaterCell;
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

/**
 * @brief A fresh, never-reused key into the process-wide TerraStore.
 *
 * sync_field() (called by apply_to()) routes every terrain through TerraStore::acquire(), keyed
 * by `data_file` -- a store shared by the whole process, not per-test. Two terrains sharing a key
 * would alias the SAME TerraData, which is exactly what each test here must not do.
 */
std::string unique_data_file() {
    static int counter = 0;
    return "test/map_generator_test_" + std::to_string(++counter) + ".terra";
}

/** @brief A tiles-mode terrain with nothing in it yet; sync_field() (inside apply_to()) fills `field`. */
void make_tile_terrain(TerrainComponent& t) {
    t.mode = TerrainMode::Tiles;
    t.params.world_tiles = 160;
    t.params.tile_size = 1.0f;
    t.params.height_step = 1.0f;
    t.params.height_scale = 32.0f;
    t.params.chunk_size = 16;
    t.data_file = unique_data_file();
}

/** @brief A heightmap-mode terrain with a flat plane and no paint; height_sub 2, so it has a real
 *         height plane (height_sub 1 would mean no plane at all -- a terraced read instead). */
void make_smooth_terrain(TerrainComponent& t) {
    t.mode = TerrainMode::Heightmap;
    t.heightmap_size = glm::vec2(160.0f);
    t.heightmap_height = 32.0f;
    t.heightmap_chunk_size = 16.0f;
    t.height_sub = 2;
    t.heightmap_resolution = 321;    // 160 tiles * height_sub 2 + 1
    t.heightmap_splat_resolution = 160;
    t.data_file = unique_data_file();
}

/** @brief Runs an apply to completion (synchronous bake, no job engine). A heightmap target's
 *         water is folded in by a budgeted sweep that can take several poll_apply() calls to
 *         finish (HeightmapTerrain::chunk_budget_ms a frame) -- drive it to completion rather
 *         than assuming one call lands it, same as a real scene's MapGeneratorSystem would
 *         across several frames. */
bool apply(MapGeneratorComponent& g, TerrainComponent& t) {
    if (!g.apply_to(t, nullptr)) return false;
    for (int i = 0; i < 1'000'000 && g.applying(); ++i) {
        if (g.poll_apply()) return true;
    }
    return !g.applying();
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

    require(!t.field.empty(), "tiles: the grid is filled");
    expect(t.field.geometry().tiles_x == 160 && t.field.geometry().tiles_y == 160,
           "tiles: at the size the generator asked for");
    expect(t.applied_map != nullptr, "tiles: the terrain keeps the map it was given");

    int wet = 0, dry = 0, ocean = 0, river = 0;
    std::int16_t lowest = 32767, highest = -32768;
    std::map<WaterKind, int> kinds;
    for (int y = 0; y < t.field.geometry().tiles_y; ++y) {
        for (int x = 0; x < t.field.geometry().tiles_x; ++x) {
            const TileCell& r = t.field.data()->at(x, y);
            lowest = std::min(lowest, r.steps);
            highest = std::max(highest, r.steps);
            if (r.wet()) {
                ++wet;
                if (static_cast<WaterKind>(r.water_kind) == WaterKind::Ocean) ++ocean;
                if (static_cast<WaterKind>(r.water_kind) == WaterKind::River) ++river;
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
    for (int y = 0; y < t.field.geometry().tiles_y; ++y) {
        for (int x = 0; x < t.field.geometry().tiles_x; ++x) {
            const TileCell& r = t.field.data()->at(x, y);
            if (static_cast<WaterKind>(r.water_kind) == WaterKind::River && r.flow >= 1 && r.flow <= 8) ++with_flow;
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
    require(t.field.data() != nullptr, "heightmap: the plane is there");

    float lowest = 1e9f, highest = -1e9f;
    for (const float h : t.field.data()->heights()) {
        lowest = std::min(lowest, h);
        highest = std::max(highest, h);
    }
    expect(highest - lowest > 1.0f,
           "heightmap: the plane has relief (" + std::to_string(highest - lowest) + " m)");
    expect(highest <= 32.0f + 1e-3f, "heightmap: ...within the terrain's height");

    // Heights are continuous, not snapped to a step: a terraced bake would give a handful of
    // distinct values over a whole world, and this is the difference the smooth path exists for.
    std::map<int, int> millimetre_buckets;
    for (const float h : t.field.data()->heights()) ++millimetre_buckets[static_cast<int>(h * 1000.0f)];
    expect(millimetre_buckets.size() > 500,
           "heightmap: " + std::to_string(millimetre_buckets.size()) + " distinct heights, so not terraced");

    // Layers: the generated surface is painted, not left blank.
    int painted = 0;
    for (int j = 0; j < t.field.data()->layer_ny(); ++j) {
        for (int i = 0; i < t.field.data()->layer_nx(); ++i) {
            int total = 0;
            for (int l = 0; l < toy::world::k_heightmap_layer_count; ++l) total += t.field.data()->layer_at(l, i, j);
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
    for (int y = 0; y < ta.field.geometry().tiles_y && same; ++y) {
        for (int x = 0; x < ta.field.geometry().tiles_x; ++x) {
            if (ta.field.data()->at(x, y).steps != tb.field.data()->at(x, y).steps) { same = false; break; }
            if (ta.field.data()->at(x, y).steps != tc.field.data()->at(x, y).steps) differs = true;
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

/**
 * @brief One generator applied to a tile terrain AND a heightmap terrain at matched sizes: the
 *        two describe the same world, not two independent samples of one seed.
 */
COOPA_TEST(one_map_gives_both_modes_the_same_ground_and_field) {
    using toy::world::TerrainLayer;
    using toy::world::TileKind;

    MapGeneratorComponent g;
    generate(g);

    TerrainComponent ta;
    make_tile_terrain(ta);
    TerrainComponent tb;
    make_smooth_terrain(tb);
    // Named in TileKind order, so a texel's painted layer can be checked against its own kind
    // by name -- the fallback unified_terrain_demo's `salt` drop exists to avoid.
    for (const char* name : {"grass", "stone", "sand", "snow", "ice", "moss", "clay", "ash"}) {
        TerrainLayer layer;
        layer.name = name;
        tb.layers.push_back(layer);
    }

    require(apply(g, ta), "both modes: the tile apply ran");
    require(apply(g, tb), "both modes: the heightmap apply ran");
    require(ta.applied_map != nullptr && tb.applied_map != nullptr,
            "both modes: both terrains kept the map they were given");

    const TerraGeometry ga = ta.field.geometry();
    const TerraGeometry gb = tb.field.geometry();
    expect(ga.tiles_x == gb.tiles_x && ga.tiles_y == gb.tiles_y, "both modes: the same tile grid");
    expect(ga.size() == gb.size(), "both modes: the same world extent");
    expect(ga.chunk_tiles == gb.chunk_tiles, "both modes: the same chunking");

    // Ground: at tile centres away from any shoreline, the terrace (tiles) and the continuous
    // plane (heightmap) should agree within a tolerance driven by two real, documented sources
    // of slop rather than noise in the test itself:
    //  - settle_water_rounding() (terrain_water.cpp) banks a DRY tile up to its wet neighbour's
    //    terrace -- a tile-mode-only reconciliation the heightmap path never runs. Tiles within
    //    one tile of water are excluded outright (the accepted non-parity on
    //    unified_terrain_demo's shoreline-depth row), rather than loosening the tolerance for
    //    everyone to cover it.
    //  - at this height_sub the height plane carries a handful of samples per tile, corner-
    //    aligned with its neighbours; ground_at_tile() bilinearly reconstructs the CENTRE from
    //    the nearest four. The tile path samples the centre directly. mapcoopa's fractal detail
    //    noise is not linear between those corners, so the reconstruction and the direct sample
    //    can differ by a fraction of the world's vertical scale even with no bug at all -- this
    //    is what raising `height_sub` further exists to shrink.
    const float tolerance = ga.height_step * 0.5f + 0.15f * gb.height;
    const auto near_water = [&](glm::ivec2 tile) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const glm::ivec2 n = tile + glm::ivec2(dx, dy);
                if (n.x < 0 || n.y < 0 || n.x >= ga.tiles_x || n.y >= ga.tiles_y) continue;
                if (ta.field.water_at(n).wet() || tb.field.water_at(n).wet()) return true;
            }
        }
        return false;
    };
    int compared = 0, agreed = 0;
    for (int ty = 0; ty < ga.tiles_y; ty += 7) {
        for (int tx = 0; tx < ga.tiles_x; tx += 7) {
            const glm::ivec2 tile(tx, ty);
            if (near_water(tile)) continue;
            const float tiled = ta.field.ground_at_tile(tile);
            const float smooth = tb.field.ground_at_tile(tile);
            ++compared;
            if (std::fabs(tiled - smooth) <= tolerance) ++agreed;
        }
    }
    expect(compared > 0, "both modes: sampled at least one tile centre");
    expect(agreed == compared,
           "both modes: ground agrees within tolerance at every sampled tile (" +
           std::to_string(agreed) + "/" + std::to_string(compared) + ")");

    // Biome: the same map, so the same point reports the same biome through either sampler --
    // what would break if apply_to() ever built a second, independently-derived config.
    int biome_checked = 0, biome_same = 0;
    for (int ty = 0; ty < ga.tiles_y; ty += 11) {
        for (int tx = 0; tx < ga.tiles_x; tx += 11) {
            const auto sa = ta.applied_map->sample_surface(tx + 0.5, ty + 0.5);
            const auto sb = tb.applied_map->sample_surface(tx + 0.5, ty + 0.5);
            ++biome_checked;
            if (sa.biome == sb.biome) ++biome_same;
        }
    }
    expect(biome_checked > 0, "both modes: sampled at least one point for biome");
    expect(biome_same == biome_checked,
           "both modes: every sampled point reports the same biome through both samplers");

    // Layer fallback: a texel resolves to the grass layer (index 0) only if its own kind really
    // is grass -- the regression a kind missing from `layers:` would otherwise hide silently.
    int off_grass_checked = 0, off_grass_correct = 0;
    const double map_tiles =
        static_cast<double>(tb.applied_map->params().tiles_for_grid(g.generated_config().grid_size));
    const int sw = tb.field.data()->layer_nx(), sh = tb.field.data()->layer_ny();
    for (int j = 0; j < sh; j += 13) {
        for (int i = 0; i < sw; i += 13) {
            const double u = (static_cast<double>(i) + 0.5) / sw;
            const double v = (static_cast<double>(j) + 0.5) / sh;
            const auto s = tb.applied_map->sample_surface(u * map_tiles, v * map_tiles);
            if (s.top == TileKind::Grass) continue;
            ++off_grass_checked;
            if (tb.field.data()->layer_at(0, i, j) == 0) ++off_grass_correct;
        }
    }
    expect(off_grass_checked > 0, "both modes: sampled at least one non-grass texel");
    expect(off_grass_correct == off_grass_checked,
           "both modes: a non-grass kind never falls back to the grass layer (" +
           std::to_string(off_grass_correct) + "/" + std::to_string(off_grass_checked) + ")");
}

/**
 * @brief Every tile the bake calls wet renders wet in a heightmap terrain too, not just a tile
 *        one.
 *
 * Tile mode trusts the baked `WaterCell::wet()` unconditionally (`TerrainField::set_water()`'s
 * tile branch, `terrain_field.cpp`). Heightmap mode re-derives wet/dry from a fresh ground sample
 * instead (`:291-292`), and that ground sample is read at the tile CENTRE while the bake's own
 * heights were written at tile CORNERS, a half-tile offset apart on the same noise-shaped
 * surface -- so a tile mapcoopa calls wet can read a hair of ground above its own water level and
 * drop to dry. Two modes baked from the one map should agree on which tiles are wet; this is
 * `unified_terrain_demo`'s own generator config (large enough, and the exact one, for the offset
 * to actually bite -- the small synthetic fixture above is not).
 */
COOPA_TEST(every_tile_the_map_calls_wet_renders_wet_on_a_heightmap_too) {
    const std::string path =
        std::string(ROOT_DIR) + "/assets/scenes/terrain/unified_terrain_demo/scene.yaml";
    const fkyaml::node doc = coopa::yaml::load_document(path);
    fkyaml::node gen_node;
    for (const auto& obj : doc.at("scene").at("root_objects")) {
        if (obj.at("name").get_value<std::string>() != "world") continue;
        for (const auto& c : obj.at("components")) {
            if (c.at("type").get_value<std::string>() == "MapGenerator") gen_node = c;
        }
    }
    require(!gen_node.is_null(), "wet parity: found the world's MapGenerator node");

    MapGeneratorComponent g;
    g.config = toy::mapgen::generator_config_from_node(gen_node);
    g.world_tiles = 1024;
    g.height_scale = 64.0f;
    g.begin(nullptr);
    require(g.ready(), "wet parity: the real generator config built");

    TerrainComponent ta;
    ta.mode = TerrainMode::Tiles;
    ta.params.world_tiles = 1024;
    ta.params.tile_size = 1.0f;
    ta.params.height_step = 1.0f;
    ta.params.height_scale = 64.0f;
    ta.params.chunk_size = 32;
    ta.data_file = unique_data_file();

    TerrainComponent tb;
    tb.mode = TerrainMode::Heightmap;
    tb.heightmap_size = glm::vec2(1024.0f);
    tb.heightmap_height = 64.0f;
    tb.heightmap_chunk_size = 32.0f;
    tb.height_sub = 2;
    tb.heightmap_resolution = 2049;   // 1024 tiles * height_sub 2 + 1
    tb.heightmap_splat_resolution = 1024;
    tb.data_file = unique_data_file();

    require(apply(g, ta), "wet parity: tile apply ran");
    require(apply(g, tb), "wet parity: heightmap apply ran");

    const TerraGeometry ga = ta.field.geometry();
    require(ga.tiles_x == tb.field.geometry().tiles_x && ga.tiles_y == tb.field.geometry().tiles_y,
            "wet parity: both modes share the same tile grid");

    int tile_wet = 0, heightmap_wet = 0, mismatched = 0;
    for (int ty = 0; ty < ga.tiles_y; ++ty) {
        for (int tx = 0; tx < ga.tiles_x; ++tx) {
            const glm::ivec2 tile(tx, ty);
            const bool wa = ta.field.water_at(tile).wet();
            const bool wb = tb.field.water_at(tile).wet();
            if (wa) ++tile_wet;
            if (wb) ++heightmap_wet;
            if (wa != wb) ++mismatched;
        }
    }
    expect(tile_wet > 0, "wet parity: the map has water to compare");
    expect(mismatched == 0,
           "wet parity: tile-mode wet " + std::to_string(tile_wet) + ", heightmap-mode wet " +
           std::to_string(heightmap_wet) + ", mismatched " + std::to_string(mismatched));
}

/**
 * @brief Grading a wet tile's bed moves it by millimetres almost everywhere, never by the old
 *        binary clamp's flat metre.
 *
 * The regression test for the clamp `grade_water_shore()` replaced: it dug every wet tile's four
 * corners down to exactly `level - height_step` (1 m here) regardless of what mapcoopa's own lake
 * and shore grading had already done, overwriting a real, varying shelf with one flat pit depth
 * repeated over every wet tile in the world. A raw adjacent-sample bound does not work here --
 * this generator's real coastline has genuine cliffs metres tall one tile wide, so a large step
 * at a shoreline is not by itself a bug. What the old clamp actually left behind was a DEFICIT
 * (how much a tile's own corner-mean had to drop to satisfy `TerrainField::set_water()`'s dryness
 * test) pinned at a flat 1 m almost everywhere; the fix's deficit should instead track whatever
 * `shore_clearance` plus the corner-vs-centre sampling offset actually needs, which this real,
 * noise-shaped map keeps to a few centimetres.
 */
COOPA_TEST(grading_moves_the_bed_by_millimetres_not_a_flat_metre) {
    const std::string path =
        std::string(ROOT_DIR) + "/assets/scenes/terrain/unified_terrain_demo/scene.yaml";
    const fkyaml::node doc = coopa::yaml::load_document(path);
    fkyaml::node gen_node;
    for (const auto& obj : doc.at("scene").at("root_objects")) {
        if (obj.at("name").get_value<std::string>() != "world") continue;
        for (const auto& c : obj.at("components")) {
            if (c.at("type").get_value<std::string>() == "MapGenerator") gen_node = c;
        }
    }
    require(!gen_node.is_null(), "stair-step: found the world's MapGenerator node");

    MapGeneratorComponent g;
    g.config = toy::mapgen::generator_config_from_node(gen_node);
    g.world_tiles = 1024;
    g.height_scale = 64.0f;
    g.begin(nullptr);
    require(g.ready(), "stair-step: the real generator config built");

    TerrainComponent tb;
    tb.mode = TerrainMode::Heightmap;
    tb.heightmap_size = glm::vec2(1024.0f);
    tb.heightmap_height = 64.0f;
    tb.heightmap_chunk_size = 32.0f;
    tb.height_sub = 2;
    tb.heightmap_resolution = 2049;   // 1024 tiles * height_sub 2 + 1
    tb.heightmap_splat_resolution = 1024;
    tb.data_file = unique_data_file();
    require(apply(g, tb), "stair-step: heightmap apply ran");

    const TerraData& h = *tb.field.data();
    const TerraGeometry geo = tb.field.geometry();
    require(h.height_nx() == geo.tiles_x * geo.height_sub + 1 && h.height_ny() == geo.tiles_y * geo.height_sub + 1,
            "stair-step: the plane follows height_sub corners-per-tile");

    // For every wet tile, how far its own corner-mean had to drop below its water level to pass
    // set_water()'s dryness test -- the quantity grade_water_shore() controls directly. This is
    // NOT depth (the open sea is legitimately tens of metres deep, and a tile whose bed was
    // already graded well clear of its level has a deficit near zero): the signal to watch is
    // not its typical size but whether a huge FRACTION of wet tiles land on one exact, flat
    // value, which is what the old clamp -- `level - height_step` on every wet tile regardless
    // of what the bed already did -- left behind.
    int wet_tiles = 0, near_old_clamp = 0;
    constexpr float k_old_clamp = 1.0f;     // the removed clamp's flat drop (height_step here)
    constexpr float k_old_clamp_slack = 0.02f;
    for (int ty = 0; ty < geo.tiles_y; ++ty) {
        for (int tx = 0; tx < geo.tiles_x; ++tx) {
            const WaterCell w = tb.field.water_at(glm::ivec2(tx, ty));
            if (!w.wet()) continue;
            ++wet_tiles;
            // The mean over the tile's WHOLE corner block (height_sub + 1 samples a side) --
            // exactly what grade_water_shore() lowers together (terrain_water.cpp).
            double sum = 0.0;
            int n = 0;
            for (int dy = 0; dy <= geo.height_sub; ++dy) {
                for (int dx = 0; dx <= geo.height_sub; ++dx) {
                    sum += h.height_at(tx * geo.height_sub + dx, ty * geo.height_sub + dy);
                    ++n;
                }
            }
            const float mean = static_cast<float>(sum / n);
            const float deficit = w.level - mean;
            if (std::abs(deficit - k_old_clamp) <= k_old_clamp_slack) ++near_old_clamp;
        }
    }
    expect(wet_tiles > 0, "stair-step: the map has water to check");
    // A small fraction landing near 1 m by chance (a genuinely 1 m-deep corner, say) is fine; the
    // old clamp pinned nearly ALL of them there, which this bound would catch by a wide margin.
    const float near_old_clamp_frac = static_cast<float>(near_old_clamp) / static_cast<float>(std::max(1, wet_tiles));
    expect(near_old_clamp_frac < 0.05f,
           "stair-step: " + std::to_string(near_old_clamp) + " / " + std::to_string(wet_tiles) +
           " wet tiles sit at a deficit within " + std::to_string(k_old_clamp_slack) +
           " m of the old clamp's flat " + std::to_string(k_old_clamp) + " m");
}

/**
 * @brief An enclosed bay reported shallow ("the lake by the ocean on the left" -- about one block
 *        deep on the tile flavours) gets a real basin, not just whatever the open sea's own rank
 *        remap happened to leave under it.
 *
 * `PassWater`'s flood fill gives the sea and any inlet off it the same `ocean` flag the instant
 * they touch through even one strait tile, so `assign_water_levels()` (water_levels.h) used to
 * skip carving a bed under a lagoon exactly as it skips the open sea -- correctly for the sea,
 * whose depth is the elevation pass's job, but wrongly for the lagoon, which has no bathymetry of
 * its own and so sat at whatever the rank remap happened to leave it: near a shore, barely under
 * `sea_level`, i.e. about one `height_step`.
 *
 * `ocean_enclosure()` tells the two apart by distance, in strait-widths, from the open-sea CORE a
 * morphological opening of the water mask finds -- scale-free, unlike the border-distance ratio
 * it replaced, which could only ever fire for a bay close to the map's edge. This bakes
 * `unified_terrain_demo`'s own config (seed 7, grid 128) -- its reported lagoon sits tens of
 * cells inland, which is exactly the case the ratio test missed -- and holds every cell the fix
 * reads as meaningfully enclosed (`strength >= 0.5`) to more than one `height_step` under its
 * surface.
 */
COOPA_TEST(an_enclosed_bay_gets_a_real_basin_not_the_open_seas_shallow_rank) {
    const std::string path =
        std::string(ROOT_DIR) + "/assets/scenes/terrain/unified_terrain_demo/scene.yaml";
    const fkyaml::node doc = coopa::yaml::load_document(path);
    fkyaml::node gen_node;
    for (const auto& obj : doc.at("scene").at("root_objects")) {
        if (obj.at("name").get_value<std::string>() != "world") continue;
        for (const auto& c : obj.at("components")) {
            if (c.at("type").get_value<std::string>() == "MapGenerator") gen_node = c;
        }
    }
    require(!gen_node.is_null(), "bay-depth: found the world's MapGenerator node");

    MapGeneratorComponent g;
    g.config = toy::mapgen::generator_config_from_node(gen_node);
    g.world_tiles = 1024;
    g.height_scale = 64.0f;
    g.begin(nullptr);
    require(g.ready(), "bay-depth: the real generator config built");

    const coopa::maps::MapGraph* graph = g.graph();
    require(graph != nullptr, "bay-depth: graph() is populated after a synchronous generate");

    const std::vector<double> enclosure = coopa::maps::ocean_enclosure(*graph);
    const float height_scale = 64.0f;
    const float height_step = 1.0f;
    const float k_shallow_rank_depth = 1.5f;   // world units -- about the old "one block" symptom
    const double k_meaningfully_enclosed = 0.5;   // strength -- past the strait's own fade-in

    int enclosed_n = 0, shallow_n = 0;
    float deepest = 0.0f;
    for (const auto& cell : graph->centers) {
        if (!cell.water || !cell.ocean) continue;
        if (enclosure[static_cast<std::size_t>(cell.index)] < k_meaningfully_enclosed) continue;
        ++enclosed_n;
        const float depth_world = static_cast<float>(cell.water_level - cell.elevation) * height_scale;
        deepest = std::max(deepest, depth_world);
        if (depth_world <= k_shallow_rank_depth * height_step) ++shallow_n;
    }
    require(enclosed_n > 0, "bay-depth: this seed's map has at least one enclosed bay to check");
    expect(shallow_n == 0,
           "bay-depth: " + std::to_string(shallow_n) + " / " + std::to_string(enclosed_n) +
           " enclosed-bay cells still read shallow-rank depth (<= " +
           std::to_string(k_shallow_rank_depth) + " world units)");
    expect(deepest > 2.0f * height_step,
           "bay-depth: the bay's deepest carved cell clears a plain one-step reading (" +
           std::to_string(deepest) + " world units)");
}

/**
 * @brief Every shallow water patch on `unified_terrain_demo`'s own map with a deep-reaching
 *        interior is actually carved there -- the invariant that would have caught the "1m deep
 *        inland lake" report before it shipped.
 *
 * Segments by SHALLOWNESS (every water cell under 1.5 world units of depth), not connectivity --
 * at tile resolution a bay is joined to the open sea through its own strait, so a flood fill over
 * "is water" would merge it into the ocean and hide it. A patch's own shoreline distance (how far
 * its deepest-reaching cell sits from the nearest dry land, in grid units) is what tells a real
 * open coastline's shallow fringe apart from a bay with an actual interior: a probe over this
 * exact seed found every legitimate shelf under 0.8 grid units and the reported bay's patches at
 * 3.3-4.1, so `k_max_open_shelf_shore` sits at 2.0, comfortably between the two.
 *
 * A patch past that threshold is not strictly required to carve literally every cell -- the
 * shallow flood-fill above does not stop at a body's own true edge, so a patch could in principle
 * span a real bay's deep reach plus a strip of ordinary open shelf beside it. In practice, on this
 * seed, every patch past the shelf threshold carves completely (`enclosed_frac` 1.0): before the
 * fix it measured exactly 0 for all of them (see the superseded
 * `an_enclosed_bay_gets_a_real_basin_not_the_open_seas_shallow_rank`'s own history); an earlier,
 * smaller `k_strait_half_width` (3.0 grid units) reached only 33-58% -- real, but still visibly
 * shallow over much of each bay -- which is why `k_min_enclosed_fraction` sits at 0.9 rather than
 * some looser bound: this invariant is meant to catch "the fix only reaches part of the bay" too,
 * not just "the fix doesn't run at all". A failure prints the offending patch's centroid (in world
 * tile coordinates, matching the editor's own camera) and its ocean/enclosure fractions so it is
 * self-diagnosing.
 */
COOPA_TEST(no_water_body_has_a_flat_shallow_interior) {
    const std::string path =
        std::string(ROOT_DIR) + "/assets/scenes/terrain/unified_terrain_demo/scene.yaml";
    const fkyaml::node doc = coopa::yaml::load_document(path);
    fkyaml::node gen_node;
    for (const auto& obj : doc.at("scene").at("root_objects")) {
        if (obj.at("name").get_value<std::string>() != "world") continue;
        for (const auto& c : obj.at("components")) {
            if (c.at("type").get_value<std::string>() == "MapGenerator") gen_node = c;
        }
    }
    require(!gen_node.is_null(), "flat-shallow: found the world's MapGenerator node");

    MapGeneratorComponent g;
    g.config = toy::mapgen::generator_config_from_node(gen_node);
    g.world_tiles = 1024;
    g.height_scale = 64.0f;
    g.begin(nullptr);
    require(g.ready(), "flat-shallow: the real generator config built");

    const coopa::maps::MapGraph* graph = g.graph();
    require(graph != nullptr, "flat-shallow: graph() is populated after a synchronous generate");

    const std::size_t n = graph->centers.size();
    const float height_scale = 64.0f;
    constexpr double k_shallow_depth_world = 1.5;     // world units -- the "one block" symptom
    constexpr double k_max_open_shelf_shore = 2.0;    // grid units -- see the doc comment above
    constexpr double k_min_enclosed_fraction = 0.9;   // see the doc comment above

    // Global shoreline distance over EVERY water cell at once (not per flood-filled body): the
    // Dijkstra relaxation shoreline_distances() (water_levels.h) does, generalised off one body.
    std::vector<double> shore(n, 0.0);
    {
        const double infinity = std::numeric_limits<double>::max();
        std::vector<double> dist(n, infinity);
        using Entry = std::pair<double, std::size_t>;
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
        for (std::size_t i = 0; i < n; ++i) {
            const coopa::maps::MapCenter& cell = graph->centers[i];
            if (!cell.water) continue;
            double s = infinity;
            for (const coopa::maps::CenterId nb : cell.neighbors) {
                const coopa::maps::MapCenter& neighbor = graph->centers[static_cast<std::size_t>(nb)];
                if (!neighbor.water) s = std::min(s, 0.5 * cell.point.distance_to(neighbor.point));
            }
            if (s < infinity) {
                dist[i] = s;
                pending.emplace(s, i);
            }
        }
        while (!pending.empty()) {
            const Entry e = pending.top();
            pending.pop();
            if (e.first > dist[e.second]) continue;
            const coopa::maps::MapCenter& cell = graph->centers[e.second];
            for (const coopa::maps::CenterId nb : cell.neighbors) {
                const std::size_t ni = static_cast<std::size_t>(nb);
                if (!graph->centers[ni].water) continue;
                const double reached = e.first + cell.point.distance_to(graph->centers[ni].point);
                if (reached < dist[ni]) {
                    dist[ni] = reached;
                    pending.emplace(reached, ni);
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) shore[i] = dist[i] == infinity ? 0.0 : dist[i];
    }

    const std::vector<double> enclosure = coopa::maps::ocean_enclosure(*graph);

    // Connected components of SHALLOW water cells (depth_world < k_shallow_depth_world).
    std::vector<bool> visited(n, false);
    std::vector<double> depth_world(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const coopa::maps::MapCenter& c = graph->centers[i];
        if (c.water) depth_world[i] = (c.water_level - c.elevation) * height_scale;
    }

    const double tiles_per_cell = 1024.0 / static_cast<double>(g.config.grid_size);   // world_tiles / grid_size

    int patch_n = 0, violations = 0;
    for (std::size_t seed_i = 0; seed_i < n; ++seed_i) {
        const coopa::maps::MapCenter& seed = graph->centers[seed_i];
        if (!seed.water || visited[seed_i] || depth_world[seed_i] >= k_shallow_depth_world) continue;

        std::vector<std::size_t> patch;
        std::queue<std::size_t> pending;
        pending.push(seed_i);
        visited[seed_i] = true;
        while (!pending.empty()) {
            const std::size_t cur = pending.front();
            pending.pop();
            patch.push_back(cur);
            for (const coopa::maps::CenterId nb : graph->centers[cur].neighbors) {
                const std::size_t ni = static_cast<std::size_t>(nb);
                if (visited[ni] || !graph->centers[ni].water) continue;
                if (depth_world[ni] >= k_shallow_depth_world) continue;
                visited[ni] = true;
                pending.push(ni);
            }
        }
        if (patch.size() < 4) continue;   // ignore noise-sized puddles
        ++patch_n;

        double sum_x = 0.0, sum_y = 0.0, ocean_n = 0.0, enc_n = 0.0, max_shore = 0.0;
        for (const std::size_t idx : patch) {
            const coopa::maps::MapCenter& c = graph->centers[idx];
            sum_x += c.point.x;
            sum_y += c.point.y;
            if (c.ocean) ocean_n += 1.0;
            if (enclosure[idx] > 0.0) enc_n += 1.0;
            max_shore = std::max(max_shore, shore[idx]);
        }
        if (max_shore <= k_max_open_shelf_shore) continue;   // an ordinary beach shelf

        const double enclosed_fraction = enc_n / static_cast<double>(patch.size());
        if (enclosed_fraction >= k_min_enclosed_fraction) continue;   // the carve engaged here

        ++violations;
        const double cx = (sum_x / static_cast<double>(patch.size())) * tiles_per_cell;
        const double cy = (sum_y / static_cast<double>(patch.size())) * tiles_per_cell;
        expect(false,
               "flat-shallow: patch " + std::to_string(patch_n) + " at world tile (" +
               std::to_string(cx) + "," + std::to_string(cy) + "), " + std::to_string(patch.size()) +
               " cells, max_shore=" + std::to_string(max_shore) + " (> " +
               std::to_string(k_max_open_shelf_shore) + "), ocean_frac=" +
               std::to_string(ocean_n / static_cast<double>(patch.size())) + " enclosed_frac=" +
               std::to_string(enclosed_fraction) + " (< " + std::to_string(k_min_enclosed_fraction) +
               ") -- a deep patch the carve never engaged on");
    }
    require(patch_n > 0, "flat-shallow: this seed's map has shallow water to check");
    expect(violations == 0,
           "flat-shallow: " + std::to_string(violations) + " / " + std::to_string(patch_n) +
           " shallow patches have an interior deeper than an open shelf should reach");
}

