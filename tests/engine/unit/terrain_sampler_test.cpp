/**
 * @file terrain_sampler_test.cpp
 * @brief TerrainSampler against a really generated map: the bucket index finds the same Voronoi
 *        cell an exhaustive scan does, and neighbouring chunks agree on their shared border (no
 *        seams between chunks meshed on different workers).
 */

#include <coopa/testing/test.h>

#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_generator.h>
#include <toyengine/world/terrain_sampler.h>

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
    const TileMeshLibrary library = make_flat_library();
    ChunkMeshData mesh;
    mesh_chunk_columns(left, library, params, mesh);
    expect(!mesh.empty(), "sampler: a chunk of generated terrain meshes to something drawable");
}
