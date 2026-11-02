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
#include <coopa/util/transform.h>
#include <coopa/yaml/document.h>
#include <glm/glm.hpp>

#include <toyengine/mapgen/map_generator_component.h>
#include <toyengine/mapgen/terrain_generator.h>
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

    const TerrainFieldGeometry ga = ta.field.geometry();
    const TerrainFieldGeometry gb = tb.field.geometry();
    expect(ga.tiles_x == gb.tiles_x && ga.tiles_y == gb.tiles_y, "both modes: the same tile grid");
    expect(ga.size == gb.size, "both modes: the same world extent");
    expect(ga.chunk_tiles == gb.chunk_tiles, "both modes: the same chunking");

    // Ground: at tile centres away from any shoreline, the terrace (tiles) and the continuous
    // plane (heightmap) should agree within a tolerance driven by two real, documented sources
    // of slop rather than noise in the test itself:
    //  - settle_water_rounding() (terrain_water.cpp) banks a DRY tile up to its wet neighbour's
    //    terrace -- a tile-mode-only reconciliation the heightmap path never runs. Tiles within
    //    one tile of water are excluded outright (the accepted non-parity on
    //    unified_terrain_demo's shoreline-depth row), rather than loosening the tolerance for
    //    everyone to cover it.
    //  - at height_sub 1 the height plane carries one SAMPLE per tile, taken at the tile's
    //    CORNER; ground_at_tile() then bilinearly reconstructs the CENTRE from its four corners.
    //    The tile path samples the centre directly. mapcoopa's fractal detail noise is not
    //    linear between those corners, so the reconstruction and the direct sample can differ by
    //    a fraction of the world's vertical scale even with no bug at all -- this is what a
    //    `height_sub` above 1 exists to shrink.
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
    const int sw = tb.data->splat_w(), sh = tb.data->splat_h();
    for (int j = 0; j < sh; j += 13) {
        for (int i = 0; i < sw; i += 13) {
            const double u = (static_cast<double>(i) + 0.5) / sw;
            const double v = (static_cast<double>(j) + 0.5) / sh;
            const auto s = tb.applied_map->sample_surface(u * map_tiles, v * map_tiles);
            if (s.top == TileKind::Grass) continue;
            ++off_grass_checked;
            if (tb.data->splat_at(0, i, j) == 0) ++off_grass_correct;
        }
    }
    expect(off_grass_checked > 0, "both modes: sampled at least one non-grass texel");
    expect(off_grass_correct == off_grass_checked,
           "both modes: a non-grass kind never falls back to the grass layer (" +
           std::to_string(off_grass_correct) + "/" + std::to_string(off_grass_checked) + ")");
}

/** @brief TEMPORARY diagnostic: march the demo camera's actual view rays (several pitches
 *         across its vertical FOV) out to the far clip, comparing where each mode's ground
 *         first occludes the ray -- checks for a FAR-FIELD relief mismatch the near-field
 *         (camera-tile-neighbourhood) diagnostic could not see. */
COOPA_TEST(zzz_diagnose_unified_terrain_demo_view_rays) {
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
    require(!gen_node.is_null(), "diag: found the world's MapGenerator node");

    MapGeneratorComponent g;
    g.config = toy::mapgen::generator_config_from_node(gen_node);
    g.world_tiles = 1024;
    g.height_scale = 64.0f;
    g.begin(nullptr);
    require(g.ready(), "diag: the real generator config built");

    TerrainComponent ta;
    ta.mode = TerrainMode::Tiles;
    ta.params.world_tiles = 1024;
    ta.params.tile_size = 1.0f;
    ta.params.height_step = 1.0f;
    ta.params.height_scale = 64.0f;
    ta.params.chunk_size = 32;
    ta.tiles = std::make_shared<toy::world::TileMapData>();

    TerrainComponent tb;
    tb.mode = TerrainMode::Heightmap;
    toy::world::HeightmapGeometry hg;
    hg.size = glm::vec2(1024.0f);
    hg.height = 64.0f;
    hg.chunk_size = 32.0f;
    hg.resolution = 1025;
    hg.splat_resolution = 1024;
    tb.data = std::make_shared<toy::world::HeightmapData>(hg.sanitized());

    require(apply(g, ta), "diag: tile apply ran");
    require(apply(g, tb), "diag: heightmap apply ran");

    // The real camera.yaml Transform: position {470,430,95}, rotation {42,0,0}, fov 60.
    const glm::vec3 cam_pos(470.0f, 430.0f, 95.0f);
    const float fov = 60.0f, half_fov = fov * 0.5f;
    const float center_pitch = 42.0f;

    std::cout << "=== view ray march, camera (470,430,95) fov 60 ===\n";
    for (float dp : {-half_fov, -half_fov * 0.5f, 0.0f, half_fov * 0.5f, half_fov}) {
        const float pitch = center_pitch + dp;   // top of frame = +half_fov (see forward below)
        coopa::util::Transform t;
        t.set_position(cam_pos);
        t.set_rotation(glm::vec3(pitch, 0.0f, 0.0f));
        const glm::vec3 forward = glm::normalize(-glm::vec3(t.get_world_matrix()[2]));

        float hit_t_a = -1.0f, hit_t_b = -1.0f;
        for (float dist = 5.0f; dist <= 600.0f; dist += 5.0f) {
            const glm::vec3 p = cam_pos + forward * dist;
            const glm::ivec2 tile(static_cast<int>(std::lround(p.x)), static_cast<int>(std::lround(p.y)));
            if (hit_t_a < 0.0f && p.z <= ta.field.ground_at_tile(tile)) hit_t_a = dist;
            if (hit_t_b < 0.0f && p.z <= tb.field.ground_at_tile(tile)) hit_t_b = dist;
            if (hit_t_a >= 0.0f && hit_t_b >= 0.0f) break;
        }
        std::cout << "pitch=" << pitch << " forward=(" << forward.x << "," << forward.y << "," << forward.z
                   << ") tile_hit_dist=" << hit_t_a << " hmap_hit_dist=" << hit_t_b << "\n";
    }
    expect(true, "diag: printed");
}
