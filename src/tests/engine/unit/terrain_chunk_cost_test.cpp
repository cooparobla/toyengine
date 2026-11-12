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
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gfxcoopa/engine/data/mesh.h>
#include <toyengine/world/planet_chunk_shaper.h>
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

/** @brief One straight, corner-free cliff: the lower half of the pad at `high_steps`, the rest
 *         at 1. Every column's along-wall neighbour sits at the same height as itself, so the
 *         whole wall classifies WallEnd::Continue -- the ordinary case a coastline or a terraced
 *         plateau is mostly made of, and the one vertical wall-run merging exists for. */
ColumnPad step_pad(std::int32_t chunk_size, std::int32_t high_steps) {
    ColumnPad pad;
    pad.resize(chunk_size);
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            TileColumn& c = pad.at(x, y);
            c.steps     = (y < chunk_size / 2) ? high_steps : 1;
            c.top_kind  = TileKind::Grass;
            c.side_kind = TileKind::Dirt;
        }
    }
    return pad;
}

} // namespace

COOPA_TEST(straight_cliff_vertex_count_is_flat_in_height) {
    // soil_depth_steps = 3 (make_test_params()): a plain wall collapses to at most a soil run
    // and a stone run whatever its height, so raising the cliff should cost close to nothing.
    const TileMeshLibrary library = make_tile_library({"soft"});
    const TerrainParams   params  = make_test_params(32);
    ChunkMeshData mesh;

    mesh_chunk_styled(step_pad(params.chunk_size, 4), library, params, mesh);
    const std::size_t low_vertices = mesh.vertices.size();

    mesh_chunk_styled(step_pad(params.chunk_size, 40), library, params, mesh);
    const std::size_t high_vertices = mesh.vertices.size();

    std::printf("straight cliff: height 4 -> %zu vertices, height 40 -> %zu vertices\n",
                low_vertices, high_vertices);
    // A 10x taller cliff must not cost anywhere near 10x the vertices -- a loose 2x bound
    // catches a regression back to one stamp a cell without being a brittle exact check.
    expect(high_vertices < low_vertices * 2, "a taller straight cliff must not cost ~10x the vertices");
}

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
        build_chunk_lods(params, ChunkCoord{0, 0}, busy_pad(params.chunk_size, 7), library, mesh);
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

/**
 * @brief The complement of the border test above, for a `ChunkLod::style` level: unlike a
 *        simplified (`ratio`) level, a style level does NOT share LOD 0's vertices -- it is
 *        re-meshed from the chunk's own columns and welded on its own, then appended behind every
 *        ratio level exactly as Mesh::build_lods_() appends a hand-authored LOD
 *        (`vertex_offset = mesh.vertices.size()` before the append). Its indices are therefore
 *        local to ITS OWN appended range, not absolute into the combined buffer -- the opposite
 *        sense from a ratio level's -- which is what `vertex_offset` exists to re-base at draw
 *        time (Mesh::draw_lod_part()'s vkCmdDrawIndexed).
 */
COOPA_TEST(style_lod_level_indexes_its_own_appended_vertices) {
    const TileMeshLibrary library = make_tile_library({"soft", "soft_lod", "rock"});
    TerrainParams params = make_test_params(32);
    params.chunk_lods    = {};
    params.chunk_lods[0] = {0.40f, 0.33f};          // one ordinary ratio level
    params.chunk_lods[1].style       = 1;            // soft_lod, by index in the library above
    params.chunk_lods[1].screen_size = 0.10f;

    ChunkMeshData mesh;
    mesh_chunk_styled(busy_pad(params.chunk_size, 3), library, params, mesh);
    coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
    const std::size_t lod0_vertices = mesh.vertices.size();

    const std::vector<coopa::gfx::engine::data::MeshLod> table =
        build_chunk_lods(params, ChunkCoord{0, 0}, busy_pad(params.chunk_size, 3), library, mesh);

    expect(table.size() == 3, "LOD 0, one ratio level and one style level");
    expect(table[1].vertex_offset == 0, "the ratio level still shares LOD 0's vertices");
    expect(table[2].vertex_offset == static_cast<std::int32_t>(lod0_vertices),
           "the style level's own vertices start where LOD 0's (plus the ratio level, which added "
           "none) left off");

    const std::size_t style_vertex_count = mesh.vertices.size() - lod0_vertices;
    expect(style_vertex_count > 0, "the style level actually appended its own geometry");
    bool in_range = true;
    for (std::size_t k = table[2].first_index; k < table[2].first_index + table[2].index_count; ++k) {
        if (mesh.indices[k] >= style_vertex_count) { in_range = false; break; }
    }
    expect(in_range, "the style level's indices stay inside its own appended vertex range -- they "
                     "are local to vertex_offset, not absolute into the combined buffer");
}

/**
 * @brief Two neighbouring PLANET chunks put the same surface heights on their shared edge.
 *
 * `max_merge = 1` makes this true by construction -- no run spans a tile, so every quad sits on
 * the uniform half-tile lattice and nothing can displace a seam vertex. The test is the tripwire
 * for changing that: a merged run bent onto the sphere is a chord whose edge leaves the surface
 * its neighbours sit on, and raising it to compensate only converts the gap into a ledge. Either
 * way the seam stops agreeing, which is what this catches.
 */
COOPA_TEST(two_planet_chunks_agree_on_their_shared_seam) {
    const TileMeshLibrary library = make_tile_library({"soft", "rock"});
    TerrainParams params = make_test_params(32);
    params.planet      = true;
    params.face_tiles  = 192;
    params.tile_size   = 1.0f;
    params.height_step = 1.0f;
    params.max_merge   = 1;   // what register.cpp forces for a planet
    params.allow_kind_corner_rounding = false;   // likewise

    const std::int32_t n = params.chunk_size;
    const float span = static_cast<float>(n) * params.tile_size;
    const ColumnPad wide = busy_pad(2 * n, 9);
    auto pad_at = [&](std::int32_t cx) {
        ColumnPad out;
        out.resize(n);
        for (std::int32_t y = -1; y <= n; ++y) {
            for (std::int32_t x = -1; x <= n; ++x) out.at(x, y) = wide.at(cx * n + x, y);
        }
        return out;
    };

    // Face space, before the warp: a vertex's z becomes its radius, so agreement here is
    // agreement on the globe.
    auto seam_heights = [&](const ChunkMeshData& mesh, float plane_x) {
        std::vector<float> zs;
        for (const auto& v : mesh.vertices) {
            if (std::abs(v.position.x - plane_x) > 1e-3f) continue;
            zs.push_back(v.position.z);
        }
        std::sort(zs.begin(), zs.end());
        zs.erase(std::unique(zs.begin(), zs.end(),
                             [](float a, float b) { return std::abs(a - b) < 1e-4f; }),
                 zs.end());
        return zs;
    };

    ChunkMeshData left, right;
    mesh_chunk_styled(pad_at(0), library, params, left);
    mesh_chunk_styled(pad_at(1), library, params, right);
    const std::vector<float> a = seam_heights(left, span);
    const std::vector<float> b = seam_heights(right, 0.0f);

    auto unmatched = [](const std::vector<float>& xs, const std::vector<float>& ys) {
        std::vector<float> out;
        for (const float x : xs) {
            bool found = false;
            for (const float y : ys) {
                if (std::abs(x - y) < 1e-3f) { found = true; break; }
            }
            if (!found) out.push_back(x);
        }
        return out;
    };
    const std::vector<float> only_left  = unmatched(a, b);
    const std::vector<float> only_right = unmatched(b, a);

    std::printf("    seam heights: left %zu, right %zu, left-only %zu, right-only %zu\n",
                a.size(), b.size(), only_left.size(), only_right.size());
    if (!only_left.empty())  std::printf("      first left-only  z = %.5f\n", only_left[0]);
    if (!only_right.empty()) std::printf("      first right-only z = %.5f\n", only_right[0]);

    expect(a.size() > 4, "the seam carries a range of heights -- the pad is busy there");
    expect(only_left.empty() && only_right.empty(),
           "both chunks put the same surface heights on their shared seam -- no step to see through");
}

/**
 * @brief `smooth` rounds a field level's shading without touching a triangle of it.
 *
 * The whole point of the flag: the level still costs exactly what it did and still carries the
 * tallest-column geometry the seam rule depends on, so turning it on can never open a crack
 * between chunks at different levels. Only the normals move -- off, every one of them is an axis
 * (flat quads and vertical walls); on, the ones where a top meets a wall tilt between the two.
 */
COOPA_TEST(a_smooth_field_level_changes_only_its_normals) {
    const TerrainParams params = make_test_params(32);
    const ColumnPad     pad    = busy_pad(params.chunk_size, 7);

    ChunkMeshData hard, soft;
    mesh_chunk_field(pad, params, 4, 1, hard);
    mesh_chunk_field(pad, params, 4, 1, soft);
    smooth_field_normals(soft);

    expect(!hard.vertices.empty(), "the field level has geometry to smooth");
    expect(soft.vertices.size() == hard.vertices.size() && soft.indices == hard.indices,
           "smoothing leaves the vertex count and every triangle exactly as they were");

    bool positions_match = true;
    for (std::size_t i = 0; i < hard.vertices.size(); ++i) {
        positions_match = positions_match && soft.vertices[i].position == hard.vertices[i].position;
    }
    expect(positions_match, "smoothing moves no vertex, so the tallest-column seam rule still holds");

    // An axis-aligned normal is one of +-X/+-Y/+-Z: what a flat quad or a vertical wall emits.
    const auto axis_aligned = [](const glm::vec3& n) {
        return std::abs(std::abs(n.x) + std::abs(n.y) + std::abs(n.z) - 1.0f) < 1e-3f &&
               (std::abs(n.x) < 1e-3f || std::abs(n.y) < 1e-3f) &&
               (std::abs(n.x) < 1e-3f || std::abs(n.z) < 1e-3f) &&
               (std::abs(n.y) < 1e-3f || std::abs(n.z) < 1e-3f);
    };
    std::size_t hard_tilted = 0, soft_tilted = 0, unit_length = 0;
    for (const auto& v : hard.vertices) hard_tilted += axis_aligned(v.normal) ? 0u : 1u;
    for (const auto& v : soft.vertices) {
        soft_tilted += axis_aligned(v.normal) ? 0u : 1u;
        unit_length += std::abs(glm::length(v.normal) - 1.0f) < 1e-3f ? 1u : 0u;
    }
    std::printf("  field stride 4: %zu/%zu normals tilted hard, %zu/%zu smooth\n",
                hard_tilted, hard.vertices.size(), soft_tilted, soft.vertices.size());
    expect(hard_tilted == 0, "without the flag every normal is still an axis");
    expect(soft_tilted > 0, "with it the normals where a top meets a wall tilt between them");
    expect(unit_length == soft.vertices.size(), "every smoothed normal is still unit length");
}

/**
 * @brief What a `field` level costs at each stride, against the chunk it stands in for.
 *
 * The headline measurement for the tier: unlike a `ratio` level, its cost is a function of
 * `chunk_size / stride` and nothing else -- not of how busy the terrain is, and not of a
 * simplifier that may decline to produce it at all.
 */
COOPA_TEST(field_level_cost_falls_with_stride) {
    const TileMeshLibrary library = make_tile_library({"soft", "rock"});
    const TerrainParams   params  = make_test_params(32);
    const ColumnPad       pad     = busy_pad(params.chunk_size, 7);

    ChunkMeshData full;
    mesh_chunk_styled(pad, library, params, full);
    coopa::gfx::engine::data::Mesh::weld_and_optimize(full.vertices, full.indices);
    const std::size_t full_tris = full.indices.size() / 3;
    std::printf("  LOD 0 (soft, busy 32-tile chunk): %zu verts, %zu tris\n", full.vertices.size(), full_tris);

    std::size_t previous = full_tris;
    for (const std::int32_t stride : {1, 2, 4, 8, 32}) {
        ChunkMeshData field;
        const auto t0 = std::chrono::steady_clock::now();
        mesh_chunk_field(pad, params, stride, 1, field);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        coopa::gfx::engine::data::Mesh::weld_and_optimize(field.vertices, field.indices);
        const std::size_t tris = field.indices.size() / 3;
        std::printf("    field stride %2d, native_border 1: %5zu verts, %5zu tris (%.1f%% of LOD 0) in %.2f ms\n",
                    stride, field.vertices.size(), tris,
                    100.0 * static_cast<double>(tris) / static_cast<double>(full_tris), ms);
        expect(tris > 0, "a field level of a busy chunk has geometry");
        expect(tris <= previous, "a coarser stride is never more triangles than a finer one");
        previous = tris;
    }
    // The same strides with no full-resolution rim, which is what the coarsest tier uses.
    for (const std::int32_t stride : {8, 32}) {
        ChunkMeshData field;
        mesh_chunk_field(pad, params, stride, 0, field);
        coopa::gfx::engine::data::Mesh::weld_and_optimize(field.vertices, field.indices);
        std::printf("    field stride %2d, native_border 0: %5zu verts, %5zu tris (%.2f%% of LOD 0)\n",
                    stride, field.vertices.size(), field.indices.size() / 3,
                    100.0 * static_cast<double>(field.indices.size() / 3) / static_cast<double>(full_tris));
    }

    ChunkMeshData coarse;
    mesh_chunk_field(pad, params, 8, 1, coarse);
    coopa::gfx::engine::data::Mesh::weld_and_optimize(coarse.vertices, coarse.indices);
    expect(coarse.indices.size() / 3 < full_tris / 5,
           "a stride-8 field level is a small fraction of the chunk it stands in for");
}

/**
 * @brief The invariant the whole seam argument rests on: a field level's surface sits at or
 *        ABOVE the full mesh everywhere, never below it.
 *
 * A block takes the height of its tallest column, so the coarse surface can only ever be too
 * high. That is what makes two chunks at different levels overlap along their shared edge
 * instead of opening a hole, and it is also why nothing the full mesh carries can poke through a
 * coarse chunk.
 */
COOPA_TEST(field_level_never_sinks_below_the_full_mesh) {
    const TerrainParams params = make_test_params(32);
    const ColumnPad     pad    = busy_pad(params.chunk_size, 11);

    for (const std::int32_t stride : {1, 4, 8}) {
        ChunkMeshData field;
        mesh_chunk_field(pad, params, stride, 1, field);
        // Unwelded, so the mesher's quads are still groups of four consecutive vertices -- which
        // is what lets each tile be tested against the quad that actually covers it. (Testing a
        // top VERTEX against the tile its coordinate floors to is wrong: a corner is shared by up
        // to four tiles, and the lowest of them owns it.)
        std::size_t checked = 0, below = 0;
        float worst = 0.0f;
        for (std::int32_t ty = 0; ty < params.chunk_size; ++ty) {
            for (std::int32_t tx = 0; tx < params.chunk_size; ++tx) {
                const float cx = (static_cast<float>(tx) + 0.5f) * params.tile_size;
                const float cy = (static_cast<float>(ty) + 0.5f) * params.tile_size;
                float surface = -1e9f;
                for (std::size_t q = 0; q + 3 < field.vertices.size(); q += 4) {
                    if (field.vertices[q].normal.z < 0.9f) continue;   // tops only
                    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                    for (std::size_t k = q; k < q + 4; ++k) {
                        const glm::vec3& p = field.vertices[k].position;
                        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
                        y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
                    }
                    if (cx < x0 || cx > x1 || cy < y0 || cy > y1) continue;
                    surface = std::max(surface, field.vertices[q].position.z);
                }
                const float true_z = static_cast<float>(pad.at(tx, ty).steps) * params.height_step;
                ++checked;
                if (surface < true_z - 1e-3f) { ++below; worst = std::max(worst, true_z - surface); }
            }
        }
        std::printf("    field stride %d: %zu tiles covered, %zu where the coarse surface sinks below "
                    "the true ground (worst %.3f)\n", stride, checked, below, worst);
        expect(checked == static_cast<std::size_t>(params.chunk_size) *
                              static_cast<std::size_t>(params.chunk_size),
               "every tile is covered by a top quad -- the field level has no holes in plan");
        expect(below == 0, "no part of a field level's surface sinks below the ground it stands in for");
    }
}

/**
 * @brief Two neighbouring chunks at DIFFERENT field strides leave no vertical gap at the seam.
 *
 * The cross-chunk guarantee nothing tested before this: each chunk's boundary wall descends to
 * the neighbour's own column height read from the shared pad, while its own surface sits at or
 * above that height, so the two chunks' z-coverage at the seam always overlaps. Checked per tile
 * row, from the meshes themselves rather than from the model that produced them.
 */
/**
 * @brief A wall descends to its neighbour's own height, however deep the drop -- in BOTH meshers.
 *
 * The seam rule both meshers keep is that a surface sits at or above its neighbour's true
 * height while its wall descends TO that height, so two chunks at different levels overlap
 * instead of opening a hole. A wall that stopped short of its neighbour would leave an open
 * band the full width of the tile edge, which back-face culling renders as a see-through
 * hole -- the artefact this pins against.
 *
 * Deep drops are ordinary, not exotic. A coarse field cell is the tallest column of a block
 * up to 32 tiles across; and on a planet at `height_scale: 256` a river channel alone cuts
 * about ten steps, a sea cliff over a hundred. Depth costs neither mesher anything: a field
 * wall is two quads at any height, and the styled mesher's vertical run-merge collapses a
 * plain wall to `soil_depth_steps + 1` stamps at any height.
 */
COOPA_TEST(a_deep_wall_reaches_its_neighbours_own_height) {
    TerrainParams params = make_test_params(32);
    const std::int32_t high = 120;          // a drop far deeper than any tile piece is tall
    const ColumnPad pad = step_pad(params.chunk_size, high);
    // The cliff's foot is the low ground's own height, whatever the drop.
    const float foot = 1.0f * params.height_step;

    const auto extent = [](const ChunkMeshData& mesh) {
        float lowest = 1e9f, highest = -1e9f;
        for (const auto& v : mesh.vertices) {
            lowest  = std::min(lowest, v.position.z);
            highest = std::max(highest, v.position.z);
        }
        return std::pair<float, float>(lowest, highest);
    };

    for (const std::int32_t stride : {1, 8, 32}) {
        ChunkMeshData mesh;
        mesh_chunk_field(pad, params, stride, 0, mesh);
        expect(!mesh.vertices.empty(), "the field mesher produced geometry");
        const auto [lowest, highest] = extent(mesh);
        std::printf("    field stride %2d: wall runs %.1f..%.1f (foot %.1f)\n", stride, lowest, highest, foot);
        expect(highest > foot, "field: the cliff top is meshed");
        expect(lowest <= foot + 1e-3f, "field: the wall reaches its neighbour's own height");
    }

    // The styled mesher runs the whole drop too: its walls are what a ground-level camera
    // actually looks at, and planet_demo's river banks are where the deepest ones are.
    for (const char* set : {"voxel", "soft"}) {
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, make_tile_library({set}), params, mesh);
        expect(!mesh.vertices.empty(), "the styled mesher produced geometry");
        const auto [lowest, highest] = extent(mesh);
        std::printf("    styled %-5s : wall runs %.1f..%.1f (foot %.1f)\n", set, lowest, highest, foot);
        expect(highest > foot, std::string("styled ") + set + ": the cliff top is meshed");
        expect(lowest <= foot + 1e-3f,
               std::string("styled ") + set + ": the wall reaches its neighbour's own height");
    }
}

COOPA_TEST(two_field_chunks_overlap_on_their_shared_seam) {
    TerrainParams params = make_test_params(32);
    const std::int32_t n = params.chunk_size;
    const float span = static_cast<float>(n) * params.tile_size;

    // One source field, sampled into two adjacent pads: chunk (0,0) and chunk (1,0). Each pad's
    // skirt therefore holds the other chunk's true edge columns, which is the real situation.
    const ColumnPad wide = busy_pad(2 * n, 5);
    auto pad_at = [&](std::int32_t cx) {
        ColumnPad out;
        out.resize(n);
        for (std::int32_t y = -1; y <= n; ++y) {
            for (std::int32_t x = -1; x <= n; ++x) out.at(x, y) = wide.at(cx * n + x, y);
        }
        return out;
    };

    ChunkMeshData left, right;
    mesh_chunk_field(pad_at(0), params, 8, 1, left);    // coarse
    mesh_chunk_field(pad_at(1), params, 1, 1, right);   // fine, a different level

    // z-coverage on the shared plane, per tile row. Left's seam is its local x == span; right's
    // is its local x == 0. Walked QUAD by quad (the mesh is unwelded, so a quad is four
    // consecutive vertices) and spread over every row the quad spans -- a coarse block's top has
    // vertices only at its band edges, so collecting bare vertices per row would leave most rows
    // of a strided chunk looking empty.
    auto coverage = [&](const ChunkMeshData& mesh, float plane_x) {
        std::vector<std::pair<float, float>> rows(static_cast<std::size_t>(n), {1e9f, -1e9f});
        for (std::size_t q = 0; q + 3 < mesh.vertices.size(); q += 4) {
            float y_lo = 1e9f, y_hi = -1e9f, z_lo = 1e9f, z_hi = -1e9f;
            int on_plane = 0;
            for (std::size_t k = q; k < q + 4; ++k) {
                const glm::vec3& p = mesh.vertices[k].position;
                if (std::abs(p.x - plane_x) > 1e-3f) continue;
                ++on_plane;
                y_lo = std::min(y_lo, p.y); y_hi = std::max(y_hi, p.y);
                z_lo = std::min(z_lo, p.z); z_hi = std::max(z_hi, p.z);
            }
            // A top quad meets the plane along one edge (two corners); a wall quad lies in it
            // (four). Fewer than two corners means the quad does not reach the seam at all.
            if (on_plane < 2) continue;
            const std::int32_t first = std::clamp(
                static_cast<std::int32_t>(std::floor(y_lo / params.tile_size + 1e-4f)), 0, n - 1);
            const std::int32_t last = std::clamp(
                static_cast<std::int32_t>(std::ceil(y_hi / params.tile_size - 1e-4f)) - 1, 0, n - 1);
            for (std::int32_t y = first; y <= last; ++y) {
                auto& row = rows[static_cast<std::size_t>(y)];
                row.first  = std::min(row.first, z_lo);
                row.second = std::max(row.second, z_hi);
            }
        }
        return rows;
    };
    const auto lo = coverage(left, span);
    const auto hi = coverage(right, 0.0f);

    std::size_t rows_checked = 0, gaps = 0;
    float worst_gap = 0.0f;
    for (std::int32_t y = 0; y < n; ++y) {
        const auto& a = lo[static_cast<std::size_t>(y)];
        const auto& b = hi[static_cast<std::size_t>(y)];
        if (a.second < a.first || b.second < b.first) continue;   // a row neither side reaches
        ++rows_checked;
        // Overlapping or touching intervals leave nothing to see through.
        const float gap = std::max(0.0f, std::max(a.first - b.second, b.first - a.second));
        if (gap > 1e-3f) { ++gaps; worst_gap = std::max(worst_gap, gap); }
    }
    std::printf("    seam rows checked %zu of %d, gaps %zu (worst %.4f)\n", rows_checked, n, gaps, worst_gap);
    expect(rows_checked == static_cast<std::size_t>(n),
           "every seam row has geometry on both sides -- each rim block's top reaches the seam");
    expect(gaps == 0, "a coarse chunk and a fine neighbour overlap at every seam row -- no hole");
}

/**
 * @brief A `field` level appends its own vertices like a `style` level -- and, unlike one, is
 *        built on a PLANET, where it lands on the sphere.
 */
COOPA_TEST(field_lod_level_is_appended_and_works_on_a_planet) {
    const TileMeshLibrary library = make_tile_library({"soft", "rock"});

    for (const bool planet : {false, true}) {
        TerrainParams params = make_test_params(32);
        params.chunk_lods    = {};
        params.chunk_lods[0] = {0.40f, 0.33f};                 // one ordinary ratio level
        params.chunk_lods[1].field_stride = 8;                 // and one field level behind it
        params.chunk_lods[1].screen_size  = 0.10f;
        params.chunk_lods[1].native_border = 1;
        params.planet     = planet;
        params.face_tiles = 64;
        if (planet) {
            params.max_merge = 1;   // what register.cpp forces for a planet
            params.allow_kind_corner_rounding = false;   // likewise
        }

        // LOD 0 is left in face space here -- ChunkBuildJob::run() is what shapes it. What this
        // checks is that build_chunk_lods() shapes the FIELD level it re-meshes.
        const ColumnPad pad = busy_pad(params.chunk_size, 3);
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, library, params, mesh);
        coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
        const std::size_t lod0_vertices = mesh.vertices.size();

        const toy::world::PlanetChunkShaper globe;
        const std::vector<coopa::gfx::engine::data::MeshLod> table =
            build_chunk_lods(params, ChunkCoord{0, 0}, pad, library, mesh, 0,
                             planet ? &globe : nullptr);

        std::printf("    planet=%d: %zu levels, LOD 0 %zu verts, field level %u tris\n",
                    planet ? 1 : 0, table.size(), lod0_vertices,
                    table.size() >= 3 ? table[2].index_count / 3 : 0u);
        expect(table.size() == 3, "LOD 0, one ratio level and one field level -- on a planet too");
        expect(table[2].vertex_offset == static_cast<std::int32_t>(lod0_vertices),
               "the field level's own vertices start where LOD 0's left off");
        expect(table[2].index_count > 0, "the field level has geometry");

        bool in_range = true;
        const std::size_t appended = mesh.vertices.size() - lod0_vertices;
        for (std::size_t k = table[2].first_index; k < table[2].first_index + table[2].index_count; ++k) {
            if (mesh.indices[k] >= appended) { in_range = false; break; }
        }
        expect(in_range, "the field level's indices are local to its own appended vertex range");

        if (!planet) continue;
        // On a planet the level must have been bent onto the globe with LOD 0, not left flat in
        // face space -- every one of its vertices stands off the planet's centre.
        const float radius = params.layout().radius();
        float lowest = 1e9f, highest = 0.0f;
        for (std::size_t v = lod0_vertices; v < mesh.vertices.size(); ++v) {
            const float r = glm::length(mesh.vertices[v].position);
            lowest  = std::min(lowest, r);
            highest = std::max(highest, r);
        }
        std::printf("      field level radii %.2f..%.2f (planet radius %.2f)\n", lowest, highest, radius);
        expect(lowest > radius * 0.5f,
               "a planet's field level is warped onto the globe, not left flat in face space");
        expect(highest < radius + static_cast<float>(mesh.max_steps + 2) * params.height_step,
               "and it does not fly off it");
    }
}
