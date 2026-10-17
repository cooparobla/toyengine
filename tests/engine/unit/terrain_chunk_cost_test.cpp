/**
 * @file terrain_chunk_cost_test.cpp
 * @brief Prints what one demo-shaped chunk costs a worker -- meshing, the collision mesh's
 *        weld/adjacency/BVH, and its LOD levels -- so the cost model in
 *        toyengine/world/README.md stays a measured number, re-runnable on any machine or build.
 *
 * Informational: the assertions only sanity-check the geometry (a busy rounded 32-tile chunk is
 * substantial, not empty). No timing is asserted -- wall clock on a desktop is not this suite's
 * to control, and the point is the printout.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <tuple>
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

/**
 * @brief What the chunk's LOD levels cost to build and how much they save -- and the one thing
 *        that must hold for them: every level keeps the chunk's BOUNDARY exactly.
 *
 * That is the contract a grid of independently simplified patches rests on. Chunks pick their
 * level from their own size on screen, so neighbours routinely draw at different levels; they
 * only meet without a crack if the border vertices they share survive every level untouched
 * (Mesh::simplify_levels()'s `lock_border`). Checkable with no GPU: collect the positions on
 * the chunk's outer edge in LOD 0, then require each level to use exactly the same set.
 */
COOPA_TEST(chunk_lods_shrink_the_chunk_and_keep_its_border) {
    const TileMeshLibrary library = make_tile_library({"soft", "rock"});
    TerrainParams params = make_test_params(32);
    const float span = static_cast<float>(params.chunk_size) * params.tile_size;

    ChunkMeshData mesh;
    mesh_chunk_styled(busy_pad(params.chunk_size, 7), library, params, mesh);
    coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
    const std::size_t base_indices = mesh.indices.size();

    // On the chunk's outer edge in chunk-local space: x or y at 0 or span.
    auto on_border = [span](const glm::vec3& p) {
        const float e = 1e-3f;
        return std::abs(p.x) < e || std::abs(p.y) < e ||
               std::abs(p.x - span) < e || std::abs(p.y - span) < e;
    };
    auto border_positions = [&](std::size_t first, std::size_t count) {
        std::vector<glm::vec3> out;
        for (std::size_t k = first; k < first + count; ++k) {
            const glm::vec3& p = mesh.vertices[mesh.indices[k]].position;
            if (on_border(p)) out.push_back(p);
        }
        std::sort(out.begin(), out.end(), [](const glm::vec3& a, const glm::vec3& b) {
            return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
        });
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    };

    std::vector<coopa::gfx::engine::data::LodRequest> levels;
    for (const TerrainParams::ChunkLod& lod : params.chunk_lods) {
        if (lod.ratio <= 0.0f) break;
        levels.push_back({lod.ratio, lod.screen_size, 1.0f});
    }
    expect(!levels.empty(), "the default params carry LOD levels");

    // What the chunk job itself runs, seam locks and all.
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<coopa::gfx::engine::data::MeshLod> table =
        build_chunk_lods(params, ChunkCoord{0, 0}, mesh);
    const double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    const std::vector<glm::vec3> base_border = border_positions(0, base_indices);
    std::printf("  busy rounded 32-tile chunk: LOD 0 %zuk tris, %zu levels built in %.2f ms "
                "(%zu border positions held)\n",
                base_indices / 3 / 1000, table.size() - 1, build_ms, base_border.size());

    expect(table.size() >= 2, "the chunk gained at least one coarser level");
    expect(table[0].index_count == base_indices, "LOD 0 is still the whole chunk");
    expect(!base_border.empty(), "the chunk has a border to hold");

    for (std::size_t i = 1; i < table.size(); ++i) {
        const auto& lod = table[i];
        const double kept = static_cast<double>(lod.index_count) / static_cast<double>(base_indices);
        std::printf("    LOD %zu: %zuk tris, %.0f%% of LOD 0 (asked for %.0f%%), screen_size %.3f\n",
                    i, lod.index_count / 3 / 1000, kept * 100.0, levels[i - 1].ratio * 100.0, lod.screen_size);
        expect(lod.index_count < table[i - 1].index_count,
               "LOD " + std::to_string(i) + " is coarser than the level before it");
        expect(lod.first_index + lod.index_count <= mesh.indices.size(),
               "LOD " + std::to_string(i) + " indexes inside the buffer");
        const std::vector<glm::vec3> border = border_positions(lod.first_index, lod.index_count);
        std::vector<glm::vec3> missing;
        std::set_difference(base_border.begin(), base_border.end(), border.begin(), border.end(),
                            std::back_inserter(missing), [](const glm::vec3& a, const glm::vec3& b) {
                                return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
                            });
        if (!missing.empty()) {
            std::printf("      %zu of %zu border positions dropped, e.g. (%.3f, %.3f, %.3f)\n",
                        missing.size(), base_border.size(), missing[0].x, missing[0].y, missing[0].z);
        }
        expect(missing.empty(),
               "LOD " + std::to_string(i) + " keeps LOD 0's border exactly -- no cracks against a neighbour");
    }
    // The levels are index ranges over LOD 0's vertices: the geometry itself is not duplicated.
    expect(*std::max_element(mesh.indices.begin(), mesh.indices.end()) < mesh.vertices.size(),
           "every level indexes LOD 0's own vertices -- no geometry is duplicated");
}
