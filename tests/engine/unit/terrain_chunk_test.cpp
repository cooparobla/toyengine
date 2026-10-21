/**
 * @file terrain_chunk_test.cpp
 * @brief The terrain chunk mesher's pure half (toyengine/world/terrain_chunk.h -- free of the
 *        device and the scene, which is what lets it run on job workers), driven by the shipped
 *        voxel tile set: exposure against the pad (tops everywhere, walls exactly where a
 *        neighbour is lower, the wall clamp), square kind boundaries for a set that asks for
 *        them, byte-determinism, the chunk collision mesh built from the drawn surface, and
 *        chunk water: agreement across a shared border, and a surface that stays inside its banks.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

#include <glm/glm.hpp>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_topology.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_chunk");

using namespace toy::test;

namespace {

/// Surface area of a chunk mesh per axis direction: +-1 x, +-2 y, +-3 z (0: not axis-aligned),
/// counting only triangles whose centroid lies above `min_z`.
std::map<int, double> area_by_direction(const toy::world::ChunkMeshData& mesh, float min_z = -1e9f) {
    std::map<int, double> out;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const glm::vec3& a = mesh.vertices[mesh.indices[t]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[t + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[t + 2]].position;
        if ((a.z + b.z + c.z) / 3.0f <= min_z) continue;
        const glm::vec3 cr = glm::cross(b - a, c - a);
        const double area = 0.5 * glm::length(cr);
        if (area < 1e-9) continue;
        const glm::vec3 n = glm::normalize(cr);
        int dir = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(n[axis]) > 0.99f) dir = (axis + 1) * (n[axis] > 0.0f ? 1 : -1);
        }
        out[dir] += area;
    }
    return out;
}

/// Vertices whose blend code asks terrain_styled.frag to round a kind corner (SurfaceBlend::Top).
int top_blend_vertices(const toy::world::ChunkMeshData& mesh) {
    int n = 0;
    for (const auto& v : mesh.vertices) {
        const auto code = static_cast<std::uint32_t>(v.uv.x);
        if (((code >> 16) & 3u) == static_cast<std::uint32_t>(toy::world::SurfaceBlend::Top)) ++n;
    }
    return n;
}

} // namespace

COOPA_TEST(flat_ground_is_tops_only) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);

    // Ground level with the skirt at the same height: nothing is exposed sideways anywhere, so
    // the chunk is one flat top. This is the assertion that would fail if the pad were
    // ignored -- every border column would wall itself in.
    ChunkMeshData mesh;
    mesh_chunk_styled(make_flat_pad(4, 3), voxel_library(), params, mesh);
    const auto area = area_by_direction(mesh);
    expect(area.size() == 1 && area.count(3) == 1, "voxel chunk: flat ground faces only up");
    expect_near(area.count(3) ? area.at(3) : 0.0, 16.0, 1e-4, "voxel chunk: one tile of top per column");

    bool all_at_surface = true;
    for (const auto& v : mesh.vertices) all_at_surface = all_at_surface && v.position.z == 3.0f;
    expect(all_at_surface, "voxel chunk: the top lands at steps * height_step");
}

COOPA_TEST(walls_appear_exactly_where_a_neighbour_is_lower) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    {
        // A plateau standing 3 steps above a skirt at 0: each outward side is a 4 x 3 wall.
        ColumnPad pad = make_flat_pad(4, 3);
        for (std::int32_t i = -1; i <= 4; ++i) {
            pad.at(i, -1).steps = 0;
            pad.at(i, 4).steps  = 0;
            pad.at(-1, i).steps = 0;
            pad.at(4, i).steps  = 0;
        }
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, voxel_library(), params, mesh);
        const auto area = area_by_direction(mesh);
        bool walls = true;
        for (int dir : {1, -1, 2, -2}) walls = walls && area.count(dir) && std::abs(area.at(dir) - 12.0) < 1e-3;
        expect(walls, "voxel chunk: each outward side of the plateau is a 4 x 3 wall");
        // Up-facing area on the plateau only: the convex corners' feet lie on the ground below.
        const auto top = area_by_direction(mesh, 0.5f);
        expect(top.count(3) && std::abs(top.at(3) - 16.0) < 1e-3,
               "voxel chunk: ...under a 4 x 4 top (" + std::to_string(top.count(3) ? top.at(3) : 0.0) + " up-facing)");
        expect(area.count(0) == 0, "voxel chunk: square blocks have no slanted faces");
    }
    {
        // One column raised by 2 in a flat field: four 1 x 2 walls, and nothing else changes.
        ColumnPad pad = make_flat_pad(4, 1);
        pad.at(2, 2).steps = 3;
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, voxel_library(), params, mesh);
        auto area = area_by_direction(mesh);
        bool walls = true;
        for (int dir : {1, -1, 2, -2}) walls = walls && area.count(dir) && std::abs(area.at(dir) - 2.0) < 1e-3;
        expect(walls, "voxel chunk: a raised column exposes all four sides, two steps each");

        // The clamp is a cap on the wall, not on the column: the top stays where it was.
        TerrainParams clamped = params;
        clamped.max_wall_steps = 1;
        ChunkMeshData clamped_mesh;
        mesh_chunk_styled(pad, voxel_library(), clamped, clamped_mesh);
        area = area_by_direction(clamped_mesh);
        walls = true;
        for (int dir : {1, -1, 2, -2}) walls = walls && area.count(dir) && std::abs(area.at(dir) - 1.0) < 1e-3;
        expect(walls, "voxel chunk: max_wall_steps caps a wall's height");
    }
}

COOPA_TEST(voxel_set_keeps_kind_boundaries_square) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    // A level checkerboard of grass and sand: every tile is a convex corner of its own kind.
    ColumnPad pad = make_flat_pad(4, 2);
    for (std::int32_t y = -1; y <= 4; ++y)
        for (std::int32_t x = -1; x <= 4; ++x) pad.at(x, y).top_kind = ((x + y) & 1) ? TileKind::Sand : TileKind::Grass;

    ChunkMeshData voxel, round;
    mesh_chunk_styled(pad, voxel_library(), params, voxel);
    mesh_chunk_styled(pad, make_tile_library({"round"}), params, round);
    expect(top_blend_vertices(round) > 0, "kind corners: the round set rounds the checkerboard's corners");
    expect(top_blend_vertices(voxel) == 0, "kind corners: the voxel set (round_kind_corners: false) keeps them square");
}

COOPA_TEST(meshing_is_byte_deterministic) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    ColumnPad pad = make_flat_pad(4, 2);
    pad.at(0, 0).steps = 5;
    pad.at(3, 1).steps = 1;
    pad.at(1, 3).steps = 7;

    // Chunks are meshed on whichever worker happens to be free, so nothing downstream may
    // depend on which one -- two runs must agree byte for byte.
    ChunkMeshData first, second;
    mesh_chunk_styled(pad, voxel_library(), params, first);
    mesh_chunk_styled(pad, voxel_library(), params, second);
    expect(first.indices == second.indices, "chunk: re-meshing the same pad gives the same indices");
    bool same = first.vertices.size() == second.vertices.size();
    for (size_t i = 0; same && i < first.vertices.size(); ++i) {
        same = std::memcmp(&first.vertices[i], &second.vertices[i], sizeof(coopa::gfx::engine::data::Vertex)) == 0;
    }
    expect(same, "chunk: re-meshing the same pad gives byte-identical vertices");
}

/** @brief Two chunks sharing a water edge assign its corners identical heights AND depths: both
 *         read the same pad columns, and the depth feeds the wave attenuation, where a border
 *         mismatch shows as a moving seam between chunks. */
COOPA_TEST(adjacent_chunks_agree_on_shared_water_edge) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    TileMapData tiles;
    tiles.resize(8, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) tiles.at(x, y).steps = 3;
    // A pond across the chunk border at x = 4, over a bed that varies tile to tile, so the
    // corner depths differ along the edge and agreement cannot pass by accident.
    for (int y = 1; y <= 2; ++y) {
        for (int x = 2; x <= 5; ++x) {
            TileRecord& r = tiles.at(x, y);
            r.steps = static_cast<std::int16_t>((x + y) % 3);
            r.set_water(3);
        }
    }

    ColumnPad pad_a, pad_b;
    sample_chunk_columns(tiles, params, ChunkCoord{0, 0}, pad_a);
    sample_chunk_columns(tiles, params, ChunkCoord{1, 0}, pad_b);
    ChunkWaterData still_a, flowing_a, still_b, flowing_b;
    build_chunk_water(pad_a, params, still_a, flowing_a);
    build_chunk_water(pad_b, params, still_b, flowing_b);
    expect(still_a.depths.size() == still_a.positions.size() && still_b.depths.size() == still_b.positions.size(),
           "chunk water: one depth per corner");

    // The shared edge is chunk A's local x = chunk_size (its far column) and chunk B's local
    // x = 0. Collect each chunk's (height, depth) per edge corner, keyed by y.
    const float edge_a = static_cast<float>(params.chunk_size) * params.tile_size;
    auto collect = [](const ChunkWaterData& w, float edge_x, bool& consistent) {
        std::map<int, glm::vec2> out; // quantised y -> (z, depth)
        consistent = true;
        for (std::size_t i = 0; i < w.positions.size(); ++i) {
            if (std::fabs(w.positions[i].x - edge_x) > 1e-5f) continue;
            const int key = static_cast<int>(std::lround(w.positions[i].y * 1024.0f));
            const glm::vec2 zd(w.positions[i].z, w.depths[i]);
            auto [it, inserted] = out.emplace(key, zd);
            if (!inserted && glm::distance(it->second, zd) > 1e-5f) consistent = false;
        }
        return out;
    };
    bool ca = false, cb = false;
    const auto edge_corners_a = collect(still_a, edge_a, ca);
    const auto edge_corners_b = collect(still_b, 0.0f, cb);
    expect(ca && cb, "chunk water: coincident corners within one chunk agree with themselves");
    expect(!edge_corners_a.empty() && edge_corners_a.size() == edge_corners_b.size(),
           "chunk water: both chunks mesh the shared edge (" + std::to_string(edge_corners_a.size()) + " corners)");
    bool match = true, varied = false;
    float first_depth = -1.0f;
    for (const auto& [key, zd] : edge_corners_a) {
        const auto it = edge_corners_b.find(key);
        if (it == edge_corners_b.end() || glm::distance(it->second, zd) > 1e-5f) match = false;
        if (first_depth < 0.0f) first_depth = zd.y;
        else if (std::fabs(zd.y - first_depth) > 1e-3f) varied = true;
    }
    expect(match, "chunk water: the shared edge carries identical heights and depths in both chunks");
    expect(varied, "chunk water: ...over a bed that really varies along it");
}

/** @brief A pond with only ONE tile of bank keeps its surface inside that bank. Every flooded
 *         tile's water reaches a tile further out, under the bank, so the shoreline is the
 *         bank's own wall rather than the mesh's edge -- but that apron's outer lip lands on the
 *         far side of a one-tile bank, over ground that may be lower than the water. Left at the
 *         water level it surfaces there and the pond reads as leaking a tile outward. */
COOPA_TEST(water_stays_inside_a_one_tile_bank) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(8);
    const float ts = params.tile_size, hs = params.height_step;

    constexpr std::int32_t k_lo = 3, k_hi = 4;
    const int water_terrace = 2;
    const float surface_z = (static_cast<float>(water_terrace) - params.water_inset) * hs;
    ColumnPad pad = make_flat_pad(params.chunk_size, 1);
    for (std::int32_t y = -1; y <= params.chunk_size; ++y) {
        for (std::int32_t x = -1; x <= params.chunk_size; ++x) {
            const bool pond = x >= k_lo && x <= k_hi && y >= k_lo && y <= k_hi;
            const bool bank = !pond && x >= k_lo - 1 && x <= k_hi + 1 && y >= k_lo - 1 && y <= k_hi + 1;
            TileColumn& c = pad.at(x, y);
            if (pond) {
                c.steps = 0;
                c.water = k_water_still;
                c.water_level = water_terrace;
            } else if (bank) {
                c.steps = water_terrace;
            }
        }
    }

    ChunkWaterData still, flowing;
    build_chunk_water(pad, params, still, flowing);
    expect(flowing.positions.empty() && !still.positions.empty(),
           "chunk water: a still pond meshes into the still surface alone");

    auto corner = [&](const glm::vec3& p, bool& touches_water) {
        const std::int32_t cx = static_cast<std::int32_t>(std::lround(p.x / ts));
        const std::int32_t cy = static_cast<std::int32_t>(std::lround(p.y / ts));
        float ground = 1e9f;
        touches_water = false;
        for (std::int32_t ty = cy - 1; ty <= cy; ++ty) {
            for (std::int32_t tx = cx - 1; tx <= cx; ++tx) {
                if (tx < -1 || ty < -1 || tx > params.chunk_size || ty > params.chunk_size) continue;
                const TileColumn& c = pad.at(tx, ty);
                ground = std::min(ground, static_cast<float>(c.steps) * hs);
                touches_water = touches_water || c.water != k_water_none;
            }
        }
        return ground;
    };

    int lips = 0, exposed = 0, shoreline = 0, off_surface = 0, would_have_shown = 0;
    for (const glm::vec3& p : still.positions) {
        bool touches_water = false;
        const float ground = corner(p, touches_water);
        if (touches_water) {
            ++shoreline;
            if (std::fabs(p.z - surface_z) > 1e-4f) ++off_surface;
            continue;
        }
        ++lips;
        if (p.z > ground + 1e-4f) ++exposed;
        if (surface_z > ground + 1e-4f) ++would_have_shown;
    }

    expect(shoreline > 0 && off_surface == 0,
           "chunk water: every corner the water itself touches sits at the body's surface");
    expect(lips > 0, "chunk water: the apron has outer lips to place (" + std::to_string(lips) + ")");
    expect(would_have_shown == lips,
           "chunk water: ...all of them over ground the water level would have stood above");
    expect(exposed == 0,
           "chunk water: no apron lip rises above the ground it is buried under (" +
               std::to_string(exposed) + " of " + std::to_string(lips) + " exposed)");
}

/** @brief A bank standing at the water's own terrace keeps its apron FLAT, at the water level.
 *
 *  The apron dives only where the ground it lands on is lower than the water. Dropping it under
 *  an ordinary bank instead undercuts the bank's rounded shoulder -- which dips toward the water
 *  inside that same tile -- and uncovers the soil rim on the top step of its wall, which reads
 *  as a line drawn along every shore. The sibling test above only builds the case where the
 *  ground beyond IS lower, where either rule dives, so this is the one that pins it. */
COOPA_TEST(an_ordinary_bank_keeps_its_apron_at_the_water_level) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(8);
    const float hs = params.height_step;

    constexpr std::int32_t k_lo = 3, k_hi = 4;
    const int water_terrace = 2;
    const float surface_z = (static_cast<float>(water_terrace) - params.water_inset) * hs;
    ColumnPad pad = make_flat_pad(params.chunk_size, water_terrace);
    for (std::int32_t y = k_lo; y <= k_hi; ++y) {
        for (std::int32_t x = k_lo; x <= k_hi; ++x) {
            TileColumn& c = pad.at(x, y);
            c.steps = 0;
            c.water = k_water_still;
            c.water_level = water_terrace;
        }
    }

    ChunkWaterData still, flowing;
    build_chunk_water(pad, params, still, flowing);

    int below = 0, at_level = 0;
    float lowest = 1e9f;
    for (const glm::vec3& p : still.positions) {
        if (std::fabs(p.z - surface_z) <= 1e-4f) { ++at_level; continue; }
        ++below;
        lowest = std::min(lowest, p.z);
    }
    expect(at_level > 0, "chunk water: the pond and its apron mesh (" + std::to_string(at_level) + " corners)");
    expect(below == 0,
           "chunk water: over ground at the water's terrace every corner stays at the surface (" +
               std::to_string(below) + " dropped, lowest " + std::to_string(lowest) + " vs " +
               std::to_string(surface_z) + ")");
}

COOPA_TEST(collision_mesh_is_the_drawn_surface) {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    // A step: the left half at 1, the right half at 3.
    ColumnPad pad = make_flat_pad(4, 1);
    for (std::int32_t y = -1; y <= 4; ++y)
        for (std::int32_t x = 2; x <= 4; ++x) pad.at(x, y).steps = 3;

    for (const char* set : {"voxel", "round"}) {
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, make_tile_library({set}), params, mesh);
        coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
        const auto collision = build_chunk_collision(mesh);
        expect(collision != nullptr && collision->triangle_count() > 0,
               std::string(set) + " collision: a non-empty chunk gets a collision mesh");
        if (!collision) continue;
        expect_near(collision->bounds().max.z, 3.0f, 1e-4f, std::string(set) + " collision: reaches the upper plateau");
        expect_near(collision->bounds().min.z, 1.0f, 1e-4f, std::string(set) + " collision: down to the lower one");

        // Welded across pieces: the drawn mesh splits vertices by normal, the collision mesh by
        // position only, and pieces share edges. (Not all of them: the chunk border is open,
        // and merged plateau rectangles meet quadrant pieces in T-junctions.)
        std::size_t linked = 0, edges = 0;
        for (std::uint32_t t = 0; t < collision->triangle_count(); ++t) {
            for (int e = 0; e < 3; ++e, ++edges) {
                if (collision->triangle_neighbor(t, e) != coopa::physx::geometry::k_no_neighbor) ++linked;
            }
        }
        expect(collision->vertices().size() < mesh.vertices.size(),
               std::string(set) + " collision: welded by position (" + std::to_string(collision->vertices().size()) + " of " +
                   std::to_string(mesh.vertices.size()) + " vertices)");
        expect(linked * 3 >= edges * 2, std::string(set) + " collision: most triangle edges are shared (" +
                                            std::to_string(linked) + " of " + std::to_string(edges) + ")");
    }
    expect(build_chunk_collision(ChunkMeshData{}) == nullptr, "collision: an empty chunk has none");
}
