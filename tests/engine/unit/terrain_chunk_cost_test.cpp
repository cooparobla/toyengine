/**
 * @file terrain_chunk_cost_test.cpp
 * @brief Prints what one demo-shaped chunk costs a worker -- meshing, and the collision mesh's
 *        weld/adjacency/BVH -- so the cost model in toyengine/world/README.md stays a measured
 *        number, re-runnable on any machine or build.
 *
 * Informational: the assertions only sanity-check the geometry (a busy rounded 32-tile chunk is
 * substantial, not empty). No timing is asserted -- wall clock on a desktop is not this suite's
 * to control, and the point is the printout.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include <gfxcoopa/engine/data/mesh.h>
#include <toyengine/world/terrain_chunk.h>

#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_chunk_cost");

using coopa::test::expect;
using namespace toy::test;
using namespace toy::world;

namespace {

/** @brief The styled_tiles busy terrain: 3x3-tile blocks at 1-4 steps, 5x5 patches of kinds. */
ColumnPad busy_pad(std::int32_t chunk_size, std::uint32_t seed) {
    ColumnPad pad;
    pad.resize(chunk_size);
    auto hash = [seed](std::int32_t x, std::int32_t y) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^
                          static_cast<std::uint32_t>(y) * 19349663u ^ seed * 83492791u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return h;
    };
    auto block = [](std::int32_t v, std::int32_t size) { return v >= 0 ? v / size : (v - size + 1) / size; };
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            TileColumn& c = pad.at(x, y);
            c.steps     = 1 + static_cast<std::int32_t>(hash(block(x, 3), block(y, 3)) % 4u);
            static const TileKind kinds[] = {TileKind::Grass, TileKind::Grass, TileKind::Stone,
                                             TileKind::Sand, TileKind::Snow};
            c.top_kind  = kinds[hash(block(x, 5) + 101, block(y, 5)) % 5u];
            c.side_kind = TileKind::Dirt;
        }
    }
    return pad;
}

double median(std::vector<double> xs) {
    std::sort(xs.begin(), xs.end());
    return xs[xs.size() / 2];
}

} // namespace

COOPA_TEST(a_demo_chunk_costs_what_the_readme_says) {
    // terrain_demo's shape: rounded (soft) tops over 32-tile chunks, unit cells.
    const TileMeshLibrary library = make_tile_library({"soft", "rock"});
    TerrainParams params = make_test_params(32);

    std::vector<double> mesh_ms, weld_ms, collide_ms;
    ChunkMeshData mesh;
    std::size_t vertices = 0, triangles = 0;
    for (std::uint32_t rep = 0; rep < 24; ++rep) {
        const ColumnPad pad = busy_pad(params.chunk_size, 1 + rep);

        // ChunkBuildJob::run()'s worker steps, minus the GPU upload (no device here).
        auto t0 = std::chrono::steady_clock::now();
        mesh_chunk_styled(pad, library, params, mesh);
        auto t1 = std::chrono::steady_clock::now();
        coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
        auto t2 = std::chrono::steady_clock::now();
        const auto collision = build_chunk_collision(mesh);
        auto t3 = std::chrono::steady_clock::now();

        mesh_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        weld_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        collide_ms.push_back(std::chrono::duration<double, std::milli>(t3 - t2).count());
        vertices += mesh.vertices.size();
        triangles += mesh.indices.size() / 3;
        expect(collision != nullptr, "a busy chunk builds a collision mesh");
    }

    std::printf("  busy rounded 32-tile chunk, %zu reps: mesh %.2f ms, weld+optimize %.2f ms, "
                "collision %.2f ms (avg %zuk vertices, %zuk triangles after weld)\n",
                mesh_ms.size(), median(mesh_ms), median(weld_ms), median(collide_ms),
                vertices / mesh_ms.size() / 1000, triangles / mesh_ms.size() / 1000);

    expect(!mesh.empty(), "the busy chunk meshes to real geometry");
    expect(triangles / mesh_ms.size() > 1000, "a busy rounded chunk is thousands of triangles");
}
