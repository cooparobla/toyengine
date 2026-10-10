/**
 * @file terrain_chunk_test.cpp
 * @brief The terrain chunk mesher's pure half (toyengine/world/terrain_chunk.h -- free of the
 *        device and the scene, which is what lets it run on job workers): atlas layout, the
 *        six-faces-from-one-mesh transforms, face exposure against the pad, greedy merging
 *        preserving area, and byte-determinism.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <utility>

#include <glm/glm.hpp>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_mesh_library.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_chunk");

using namespace toy::test;

namespace {

/// Surface area of a chunk mesh per (face direction, tile kind) -- the invariant greedy
/// merging must preserve. `encoded`: UVs are TileMeshLibrary::encode_uv tile-space (greedy
/// mesher); otherwise atlas-space, decoded through the atlas grid.
std::map<std::pair<int, int>, double> area_by_direction_and_kind(const toy::world::ChunkMeshData& mesh,
                                                                 bool encoded) {
    using namespace toy::world;
    std::map<std::pair<int, int>, double> out;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const auto& a = mesh.vertices[mesh.indices[t]];
        const auto& b = mesh.vertices[mesh.indices[t + 1]];
        const auto& c = mesh.vertices[mesh.indices[t + 2]];
        const glm::vec3 cr = glm::cross(b.position - a.position, c.position - a.position);
        const double area = 0.5 * glm::length(cr);
        if (area < 1e-9) continue;
        const glm::vec3 n = glm::normalize(cr);
        int dir = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(n[axis]) > 0.5f) dir = (axis + 1) * (n[axis] > 0.0f ? 1 : -1);
        }
        const glm::vec2 uv = (a.uv + b.uv + c.uv) / 3.0f;
        int kind = 0;
        if (encoded) {
            kind = static_cast<int>(std::floor(uv.x / TileMeshLibrary::k_uv_cell_stride));
        } else {
            kind = static_cast<int>(std::floor(uv.y * k_atlas_rows)) * static_cast<int>(k_atlas_columns) +
                   static_cast<int>(std::floor(uv.x * k_atlas_columns));
        }
        out[{dir, kind}] += area;
    }
    return out;
}

} // namespace

COOPA_TEST(atlas_cells_are_disjoint_and_uvs_stay_inside_them) {
    {
        using namespace toy::world;

        for (size_t i = 0; i < k_tile_kind_count; ++i) {
            const glm::vec4 cell = atlas_cell(static_cast<TileKind>(i));
            expect(cell.x > 0.0f && cell.y > 0.0f, "atlas: cell origin is inset off the texture edge");
            expect(cell.x + cell.z < 1.0f && cell.y + cell.w < 1.0f,
                   "atlas: cell stays inside the texture");
            expect(cell.z > 0.0f && cell.w > 0.0f, "atlas: cell has positive extent");
        }

        // Disjointness is what keeps one surface from bleeding into another under NEAREST
        // filtering, and it is a property of the layout arithmetic, not of the PNG.
        bool overlap = false;
        for (size_t i = 0; i < k_tile_kind_count && !overlap; ++i) {
            const glm::vec4 a = atlas_cell(static_cast<TileKind>(i));
            for (size_t j = i + 1; j < k_tile_kind_count; ++j) {
                const glm::vec4 b = atlas_cell(static_cast<TileKind>(j));
                const bool separated = a.x + a.z <= b.x || b.x + b.z <= a.x ||
                                        a.y + a.w <= b.y || b.y + b.w <= a.y;
                if (!separated) {
                    overlap = true;
                    break;
                }
            }
        }
        expect(!overlap, "atlas: no two tile kinds share texels");
    }
    {
        using namespace toy::world;
        const TerrainParams params = make_test_params(4);
        const TileMeshLibrary library = make_flat_library();

        ColumnPad pad = make_flat_pad(4, 2);
        pad.at(1, 1).steps     = 6;          // a wall deep enough to reach the Stone band
        pad.at(2, 2).top_kind  = TileKind::Sand;
        pad.at(2, 2).side_kind = TileKind::Sand;

        ChunkMeshData mesh;
        mesh_chunk_columns(pad, library, params, mesh);

        // Every emitted UV must land inside SOME kind's cell, and never in the gutter between
        // cells -- that gutter is what a half-texel rounding error would sample.
        bool all_inside = true;
        for (const auto& v : mesh.vertices) {
            bool in_any = false;
            for (size_t i = 0; i < k_tile_kind_count; ++i) {
                const glm::vec4 cell = atlas_cell(static_cast<TileKind>(i));
                if (v.uv.x >= cell.x - 1e-5f && v.uv.x <= cell.x + cell.z + 1e-5f &&
                    v.uv.y >= cell.y - 1e-5f && v.uv.y <= cell.y + cell.w + 1e-5f) {
                    in_any = true;
                    break;
                }
            }
            all_inside = all_inside && in_any;
        }
        expect(all_inside, "chunk: every emitted UV lands inside an atlas cell");
    }
}

COOPA_TEST(face_transforms_carry_plus_z_onto_face_normals) {
    using namespace toy::world;

    // The canonical mesh faces +Z, so its transform must carry +Z onto each face's own
    // outward direction -- the one invariant the whole six-faces-from-one-mesh trick rests on.
    for (size_t i = 0; i < k_tile_face_count; ++i) {
        const TileFace face = static_cast<TileFace>(i);
        const glm::vec3 rotated =
            glm::mat3(face_transform(face)) * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 expected = face_normal(face);
        expect(glm::length(rotated - expected) < 1e-5f,
               "face transform: canonical +Z maps onto the face's outward normal");
    }

    // ...and it must be a rotation ABOUT THE CELL CENTRE, so every baked face still occupies
    // the same unit cell and can be placed by a plain translate.
    const TileMeshLibrary library = make_flat_library();
    for (size_t i = 0; i < k_tile_face_count; ++i) {
        const TileFace face = static_cast<TileFace>(i);
        bool inside = true;
        for (const auto& v : library.side(face).vertices) {
            inside = inside && v.position.x > -1e-4f && v.position.x < 1.0f + 1e-4f &&
                     v.position.y > -1e-4f && v.position.y < 1.0f + 1e-4f &&
                     v.position.z > -1e-4f && v.position.z < 1.0f + 1e-4f;
        }
        expect(inside, "face transform: the baked side still spans the [0,1]^3 cell");
    }
}

COOPA_TEST(faces_emit_exactly_where_a_neighbour_is_lower) {
    {
        using namespace toy::world;
        const TerrainParams params = make_test_params(4);
        const TileMeshLibrary library = make_flat_library();

        // Ground level with the skirt at the same height: nothing is exposed sideways anywhere,
        // so the chunk is exactly one top face per column. This is the assertion that would fail
        // if the pad were ignored -- every border column would wall itself in.
        ChunkMeshData mesh;
        mesh_chunk_columns(make_flat_pad(4, 3), library, params, mesh);
        expect(side_count(mesh) == 16, "chunk: flat ground emits one face per column and no walls");
        expect(mesh.vertices.size() == 16 * 4, "chunk: a flat quad side contributes four vertices");

        // Every top sits at the column's surface height, not at its cell floor.
        bool all_at_surface = true;
        for (const auto& v : mesh.vertices) all_at_surface = all_at_surface && v.position.z == 3.0f;
        expect(all_at_surface, "chunk: the top face lands at steps * height_step");
    }
    {
        using namespace toy::world;
        const TerrainParams params = make_test_params(4);
        const TileMeshLibrary library = make_flat_library();

        // A plateau standing 3 steps above a skirt at 0: only the border columns have a lower
        // neighbour, and only on the sides that face outward.
        ColumnPad pad = make_flat_pad(4, 3);
        for (std::int32_t i = -1; i <= 4; ++i) {
            pad.at(i, -1).steps = 0;
            pad.at(i, 4).steps  = 0;
            pad.at(-1, i).steps = 0;
            pad.at(4, i).steps  = 0;
        }

        ChunkMeshData mesh;
        mesh_chunk_columns(pad, library, params, mesh);

        // 16 tops, plus 3 steps of wall on each outward-facing side: 12 edge columns contribute
        // one side each and the 4 corners contribute two.
        const size_t expected_walls = static_cast<size_t>((12 - 4) * 1 + 4 * 2) * 3;
        expect(side_count(mesh) == 16 + expected_walls,
               "chunk: walls appear exactly where a neighbour is lower");
    }
    {
        using namespace toy::world;
        const TerrainParams params = make_test_params(4);
        const TileMeshLibrary library = make_flat_library();

        // One column raised by 2 in an otherwise flat field: four walls, two steps each, and
        // nothing else changes.
        ColumnPad pad = make_flat_pad(4, 1);
        pad.at(2, 2).steps = 3;

        ChunkMeshData mesh;
        mesh_chunk_columns(pad, library, params, mesh);
        expect(side_count(mesh) == 16 + 4 * 2, "chunk: a raised column exposes all four sides");

        // The clamp is a cap on the wall, not on the column: the top stays where it was.
        TerrainParams clamped = params;
        clamped.max_wall_steps = 1;
        ChunkMeshData clamped_mesh;
        mesh_chunk_columns(pad, library, clamped, clamped_mesh);
        expect(side_count(clamped_mesh) == 16 + 4 * 1, "chunk: max_wall_steps caps a wall's height");
    }
}

COOPA_TEST(greedy_merge_preserves_surface_area) {
    using namespace toy::world;
    TerrainParams params = make_test_params(8);
    params.soil_depth_steps = 2;
    const TileMeshLibrary library = make_flat_library();
    expect(library.side(TileFace::Top).mergeable && library.side(TileFace::North).mergeable,
           "greedy: the flat tile side is mergeable on every face");

    // Rolling terrain with plateaus (so tops can merge) and a few kinds.
    ColumnPad pad;
    pad.resize(8);
    std::mt19937 rng(1234);
    for (std::int32_t y = -1; y <= 8; ++y) {
        for (std::int32_t x = -1; x <= 8; ++x) {
            TileColumn& c = pad.at(x, y);
            c.steps     = 2 + ((x / 3 + y / 2) % 4) + static_cast<std::int32_t>(rng() % 2) * ((x + y) % 5 == 0);
            c.top_kind  = static_cast<TileKind>(rng() % 3 == 0 ? 3 : 0);   // sand or grass
            c.side_kind = (c.top_kind == TileKind::Sand) ? TileKind::Sand : TileKind::Dirt;
        }
    }

    ChunkMeshData per_tile, greedy;
    mesh_chunk_columns(pad, library, params, per_tile);
    mesh_chunk_columns_greedy(pad, library, params, greedy);

    const auto expected = area_by_direction_and_kind(per_tile, false);
    const auto actual   = area_by_direction_and_kind(greedy, true);
    bool same = expected.size() == actual.size();
    for (const auto& [key, area] : expected) {
        auto it = actual.find(key);
        same = same && it != actual.end() && std::abs(it->second - area) < 1e-3;
    }
    expect(same, "greedy: every (direction, kind) covers exactly the per-tile area");
    // This pad scatters sand tiles at random, so it merges far less than real terrain (which
    // drops ~60% in terrain_test); the invariant is only that merging never adds triangles.
    expect(greedy.indices.size() < per_tile.indices.size(),
           "greedy: fewer triangles than the per-tile mesher");
    if (coopa::test::verbose()) std::cout << "    triangles per-tile " << per_tile.indices.size() / 3 << " -> greedy "
              << greedy.indices.size() / 3 << "\n";

    ChunkMeshData again;
    mesh_chunk_columns_greedy(pad, library, params, again);
    bool identical = again.indices == greedy.indices && again.vertices.size() == greedy.vertices.size();
    for (size_t i = 0; identical && i < again.vertices.size(); ++i) {
        identical = again.vertices[i].position == greedy.vertices[i].position &&
                    again.vertices[i].uv == greedy.vertices[i].uv;
    }
    expect(identical, "greedy: deterministic -- equal pads give identical buffers");
}

COOPA_TEST(meshing_is_byte_deterministic) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    const TileMeshLibrary library = make_flat_library();

    ColumnPad pad = make_flat_pad(4, 2);
    pad.at(0, 0).steps = 5;
    pad.at(3, 1).steps = 1;
    pad.at(1, 3).steps = 7;

    // Chunks are meshed on whichever worker happens to be free, so nothing downstream may
    // depend on which one -- two runs must agree byte for byte.
    ChunkMeshData first;
    ChunkMeshData second;
    mesh_chunk_columns(pad, library, params, first);
    mesh_chunk_columns(pad, library, params, second);

    expect(first.indices == second.indices, "chunk: re-meshing the same pad gives the same indices");
    bool same = first.vertices.size() == second.vertices.size();
    for (size_t i = 0; same && i < first.vertices.size(); ++i) {
        same = std::memcmp(&first.vertices[i], &second.vertices[i],
                           sizeof(coopa::gfx::engine::data::Vertex)) == 0;
    }
    expect(same, "chunk: re-meshing the same pad gives byte-identical vertices");
}
