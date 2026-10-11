/**
 * @file terrain_chunk_test.cpp
 * @brief The terrain chunk mesher's pure half (toyengine/world/terrain_chunk.h -- free of the
 *        device and the scene, which is what lets it run on job workers), driven by the shipped
 *        voxel tile set: exposure against the pad (tops everywhere, walls exactly where a
 *        neighbour is lower, the wall clamp), square kind boundaries for a set that asks for
 *        them, byte-determinism, and the chunk collision mesh built from the drawn surface.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

#include <glm/glm.hpp>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <toyengine/world/terrain_chunk.h>
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
