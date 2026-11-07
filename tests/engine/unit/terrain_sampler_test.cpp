/**
 * @file terrain_sampler_test.cpp
 * @brief TerrainSampler against a really generated map: the bucket index finds the same Voronoi
 *        cell an exhaustive scan does, neighbouring chunks agree on their shared border (no
 *        seams between chunks meshed on different workers), and the ground under water bakes to
 *        a graded floor rather than one flat terrace.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_presets.h>
#include <toyengine/mapgen/terrain_generator.h>
#include <toyengine/mapgen/terrain_sampler.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_sampler");

using namespace toy::test;

namespace {

/** @brief A Logger that says nothing, so map generation does not bury the test output. */
class QuietLogger : public coopa::debug::Logger {
public:
    QuietLogger() : Logger("world_test") {}
    void info(const std::string&, std::string = "", bool = false, unsigned int = 0) override {}
};

} // namespace

COOPA_TEST(cell_lookup_matches_a_brute_force_scan) {
    using namespace toy::world;

    QuietLogger logger;
    coopa::maps::MapConfig config;
    config.seed      = 7;
    config.grid_size = 24;   // small on purpose: this group must stay instant
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), make_test_params());
    expect(sampler.is_built(), "sampler: a generated map builds");

    // A Voronoi cell IS the set of points nearest its site, so the bucket index has to agree
    // with an exhaustive scan at every point -- that equivalence is the whole justification
    // for the index existing.
    const coopa::maps::MapGraph& graph = sampler.graph();
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> pick(0.0, static_cast<double>(config.grid_size));

    int mismatches = 0;
    for (int i = 0; i < 2000; ++i) {
        const double x = pick(rng);
        const double y = pick(rng);

        double best = std::numeric_limits<double>::max();
        std::size_t best_index = 0;
        for (std::size_t c = 0; c < graph.centers.size(); ++c) {
            const double dx = graph.centers[c].point.x - x;
            const double dy = graph.centers[c].point.y - y;
            const double d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                best_index = c;
            }
        }
        if (sampler.cell_at(x, y).index != graph.centers[best_index].index) ++mismatches;
    }
    expect(mismatches == 0, "sampler: the bucket index finds the same cell a full scan does");
    if (mismatches != 0) std::cerr << "         " << mismatches << " of 2000 points disagreed\n";
}

COOPA_TEST(neighbouring_chunks_agree_on_their_shared_border) {
    using namespace toy::world;

    QuietLogger logger;
    coopa::maps::MapConfig config;
    config.seed              = 11;
    config.grid_size         = 24;
    config.terrain_roughness = 0.2;
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    TerrainParams params = make_test_params(8);
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);

    // Two neighbouring chunks, sampled independently exactly as two worker threads would.
    ColumnPad left;
    ColumnPad right;
    sample_chunk_columns(sampler, params, ChunkCoord{1, 1}, left);
    sample_chunk_columns(sampler, params, ChunkCoord{2, 1}, right);

    // The left chunk's +X skirt is the right chunk's first column and vice versa. If those
    // ever disagreed, each chunk would wall itself in against a neighbour that is not there,
    // and the world would show a seam at every chunk boundary.
    bool agree = true;
    for (std::int32_t y = 0; y < params.chunk_size; ++y) {
        agree = agree && left.at(params.chunk_size, y).steps == right.at(0, y).steps;
        agree = agree && right.at(-1, y).steps == left.at(params.chunk_size - 1, y).steps;
    }
    expect(agree, "sampler: adjacent chunks sample their shared border identically");

    // And the pad is genuinely load-bearing: with the neighbour's real heights, an interior
    // border column emits a +X wall only where the terrain really does step down.
    ChunkMeshData mesh;
    mesh_chunk_styled(left, voxel_library(), params, mesh);
    expect(!mesh.empty(), "sampler: a chunk of generated terrain meshes to something drawable");
}

/**
 * @brief The baked tile world has a sea floor and lake beds with real relief, graded by depth.
 *
 * mapcoopa shapes the ground under water (NaturalTerrainConfig::seabed, and a basin under every
 * lake); this is the tile grid's own view of it. What it rules out is a flooded world of
 * terraces: a bed that is the bare interpolation of the cell heights changes by a tenth of a
 * metre per tile where the land changes by two, so a whole ocean bakes to a single step.
 *
 * Measured as the share of flooded tiles standing at the same step as the tile beside them.
 */
COOPA_TEST(the_floor_under_water_has_relief_and_grades_by_depth) {
    using namespace toy::world;

    QuietLogger logger;
    // The generator a TileTerrain component makes by default -- natural terrain, which is the
    // surface the engine actually samples (see toy::mapgen::default_generator()).
    coopa::maps::MapConfig config = toy::mapgen::default_generator();
    config.seed      = 31;
    config.grid_size = 32;
    config = coopa::maps::derived_config(config);
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    // terrain_demo's vertical scale: 32 world units for the whole elevation range.
    TerrainParams params = make_test_params(16);
    params.height_scale = 32.0f;
    params.world_tiles  = config.grid_size * 16;
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);

    TerraData tiles;
    sampler.bake(tiles);
    expect(!tiles.empty(), "seabed: the world bakes");

    std::size_t flooded = 0;
    std::size_t level_with_neighbour = 0;
    std::int32_t deepest = 0;
    std::map<TileKind, std::size_t> bed_kinds;
    const float height_step = tiles.geometry().height_step;
    for (std::int32_t y = 0; y + 1 < tiles.geometry().tiles_y; ++y) {
        for (std::int32_t x = 0; x + 1 < tiles.geometry().tiles_x; ++x) {
            const TileCell& t = tiles.at(x, y);
            if (!t.wet() || static_cast<WaterKind>(t.water_kind) == WaterKind::River) continue;
            ++flooded;
            const std::int32_t water_steps = static_cast<std::int32_t>(std::lround(t.water_level / height_step));
            deepest = std::max(deepest, water_steps - t.steps);
            bed_kinds[t.top_kind()] += 1;
            const TileCell& n = tiles.at(x + 1, y);
            if (n.wet() && n.steps == t.steps) ++level_with_neighbour;
        }
    }
    expect(flooded > 1000, "seabed: the world has open water to sound");
    // A bed is smoother than a hillside, so most neighbours do share a step -- but not nearly
    // all of them. With no bed landforms at all it is 99.7%.
    const double level = static_cast<double>(level_with_neighbour) / static_cast<double>(flooded);
    expect(level < 0.96, "seabed: the floor steps rather than running flat");
    // And it is deep: the ocean reaches several steps under its own surface.
    expect(deepest >= 4, "seabed: the sea floor lies several steps under the waterline");
    // Graded, not one sand plain all the way down -- see TerrainSampler::bed_kind_for().
    expect(bed_kinds.size() >= 3, "seabed: the floor is surfaced by depth, not uniformly");
    std::cerr << "         " << flooded << " flooded tiles, " << static_cast<int>(level * 100)
              << "% level with their neighbour, deepest " << deepest << " steps, "
              << bed_kinds.size() << " bed surfaces\n";
}

/**
 * @brief A field with a height plane bakes each wet tile's level at mapcoopa's exact surface,
 *        where a terraced field rounds it to a whole step.
 *
 * mapcoopa guarantees a river's banked surface stays under both banks of the CONTINUOUS ground.
 * A terraced world can round that surface to a step and still look right -- its shoreline hides
 * inside a bank's wall either way -- but a height plane cannot: there the water is a flat sheet
 * against ground that keeps falling, so half a step of lift is half a step of water standing
 * proud of the bank, which is what `unified_terrain_demo`'s heightmap flavour showed along every
 * river. The two bakes are taken from one sampler here, so the only thing that can differ
 * between them is the rounding under test.
 */
COOPA_TEST(a_height_plane_bakes_water_levels_unrounded) {
    using namespace toy::world;

    QuietLogger logger;
    coopa::maps::MapConfig config = toy::mapgen::default_generator();
    config.seed      = 31;
    config.grid_size = 24;
    config = coopa::maps::derived_config(config);
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    TerrainParams params = make_test_params(16);
    params.height_scale = 32.0f;
    params.world_tiles  = config.grid_size * 8;
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);

    TerraData terraced;                     // no height plane: what the two tile flavours bake
    sampler.bake(terraced);
    TerraGeometry smooth_geometry;          // height_sub 2: a real, continuous height plane
    smooth_geometry.height_sub = 2;
    TerraData smooth(smooth_geometry);
    sampler.bake(smooth);
    require(!terraced.has_heights(), "levels: the terraced bake really has no height plane");
    require(smooth.has_heights(), "levels: the smooth bake really has one");

    const float step = terraced.geometry().height_step;
    const auto on_a_step = [step](float level) {
        return std::abs(level - std::round(level / step) * step) <= 1e-3f;
    };

    std::size_t terraced_wet = 0, terraced_off_step = 0;
    for (std::int32_t y = 0; y < terraced.geometry().tiles_y; ++y) {
        for (std::int32_t x = 0; x < terraced.geometry().tiles_x; ++x) {
            const TileCell& t = terraced.at(x, y);
            if (!t.wet()) continue;
            ++terraced_wet;
            if (!on_a_step(t.water_level)) ++terraced_off_step;
        }
    }
    require(terraced_wet > 200, "levels: the terraced world has water to measure");
    expect(terraced_off_step == 0, "levels: a terraced world keeps every level on a whole step");

    std::size_t wet = 0, mismatched = 0, river = 0, river_between_steps = 0;
    float worst_shift = 0.0f;
    for (std::int32_t y = 0; y < smooth.geometry().tiles_y; ++y) {
        for (std::int32_t x = 0; x < smooth.geometry().tiles_x; ++x) {
            const TileCell& t = smooth.at(x, y);
            if (!t.wet()) continue;
            ++wet;
            // record_() samples a tile at its CENTRE, so that is where the continuous surface is
            // read back from -- the same call, so an exact match is the claim, not a near one.
            const float exact = sampler.sample_surface(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5).water_surface;
            if (std::abs(t.water_level - exact) > 1e-3f) ++mismatched;
            // Only where both bakes kept the tile wet: settle_water_rounding() may have dried a
            // marginal tile on the terraced side, and that is a different difference.
            if (terraced.at(x, y).wet()) {
                worst_shift = std::max(worst_shift, std::abs(t.water_level - terraced.at(x, y).water_level));
            }
            if (static_cast<WaterKind>(t.water_kind) != WaterKind::River) continue;
            ++river;
            if (!on_a_step(t.water_level)) ++river_between_steps;
        }
    }
    require(wet > 200, "levels: the smooth world has water to measure");
    expect(mismatched == 0,
           "levels: every smooth level is mapcoopa's own surface, to the millimetre (" +
           std::to_string(mismatched) + " of " + std::to_string(wet) + " disagreed)");
    // A river is where this matters and where it shows: its surface is banked along its own
    // course, so it lands wherever the ground took it. (A sea or a lake is one flat level, which
    // can perfectly well sit on a step -- this map's sea does, so counting every wet tile here
    // would measure the ocean's single coincidence rather than the rounding under test.)
    require(river > 50, "levels: the map placed rivers to measure (" + std::to_string(river) + ")");
    expect(river_between_steps * 2 > river,
           "levels: most river levels fall between steps, where a terrace cannot (" +
           std::to_string(river_between_steps) + " of " + std::to_string(river) + ")");
    // And the rounding that is gone was never more than the half step it could ever have been.
    expect(worst_shift <= 0.5f * step + 1e-3f,
           "levels: the smooth bake differs from the terraced one by at most half a step (" +
           std::to_string(worst_shift) + ")");
}
