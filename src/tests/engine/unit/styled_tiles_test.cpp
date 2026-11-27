/**
 * @file styled_tiles_test.cpp
 * @brief Styled (shaped) terrain tiles (toyengine/world/tile_topology.h + the styled mesher) with
 *        all four shipped styles: orientation transforms are rigid, quadrants and wall ends classify
 *        congruently, plateaus stay flat, section caps appear only at style changes, rays from above
 *        and at grazing angles find no holes, and meshing is byte-deterministic.
 *        STYLED_RAY_STRESS=1 multiplies the ray counts for after changing a piece.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <coopa/asset/asset_index.h>
#include <coopa/yaml/document.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/tile_topology.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("styled_tiles");

using namespace toy::test;

namespace {

/**
 * @brief A library with all four shipped tile sets: `round` (style 0, every kind not listed),
 *        `rock` (Stone, Rock), `soft` (Sand) and `voxel` (Snow) -- so seams between every pair
 *        of sets get exercised. Built once: it is immutable, and loading forty-four YAML files
 *        per test adds up.
 */
const toy::world::TileMeshLibrary& styled_library() {
    using namespace toy::world;
    static const TileMeshLibrary library = [] {
        TileMeshLibrary lib = make_tile_library({"round", "rock", "soft", "voxel"});
        lib.set_kind_style(TileKind::Stone, 1);
        lib.set_kind_style(TileKind::Rock, 1);
        lib.set_kind_style(TileKind::Sand, 2);
        lib.set_kind_style(TileKind::Snow, 3);
        return lib;
    }();
    return library;
}

/** @brief `soft_lod` alone, every kind -- the far LOD tier's own ray-hole coverage. */
const toy::world::TileMeshLibrary& soft_lod_library() {
    static const toy::world::TileMeshLibrary library = make_tile_library({"soft_lod"});
    return library;
}

/** @brief A pad (skirt included) with every column at `steps`. */
toy::world::ColumnPad make_styled_pad(std::int32_t chunk_size, std::int32_t steps) {
    toy::world::ColumnPad pad;
    pad.resize(chunk_size);
    for (auto& column : pad.columns) column.steps = steps;
    return pad;
}

/** @brief Geometric (winding) normal of triangle `t` of a merged chunk mesh, unnormalised. */
glm::vec3 chunk_triangle_normal(const toy::world::ChunkMeshData& mesh, std::size_t t) {
    const glm::vec3& a = mesh.vertices[mesh.indices[t * 3]].position;
    const glm::vec3& b = mesh.vertices[mesh.indices[t * 3 + 1]].position;
    const glm::vec3& c = mesh.vertices[mesh.indices[t * 3 + 2]].position;
    return glm::cross(b - a, c - a);
}

/** @brief Centroid of triangle `t` of a merged chunk mesh. */
glm::vec3 chunk_triangle_centroid(const toy::world::ChunkMeshData& mesh, std::size_t t) {
    return (mesh.vertices[mesh.indices[t * 3]].position + mesh.vertices[mesh.indices[t * 3 + 1]].position +
            mesh.vertices[mesh.indices[t * 3 + 2]].position) / 3.0f;
}

/** @brief Counts triangles whose unit normal is within ~8 degrees of `dir` and that pass `where`. */
template <typename Pred>
int count_facing(const toy::world::ChunkMeshData& mesh, const glm::vec3& dir, Pred where) {
    int n = 0;
    for (std::size_t t = 0; t < mesh.indices.size() / 3; ++t) {
        const glm::vec3 normal = chunk_triangle_normal(mesh, t);
        const float length = glm::length(normal);
        if (length < 1e-9f || glm::dot(normal / length, dir) < 0.99f) continue;
        if (where(chunk_triangle_centroid(mesh, t))) ++n;
    }
    return n;
}

/**
 * @brief Ray-casts a merged chunk: the nearest triangle along the ray, or none.
 * @return The ray parameter, or a negative value for a miss; `normal` gets the winding normal.
 */
float cast_chunk_ray(const toy::world::ChunkMeshData& mesh, const glm::vec3& origin, const glm::vec3& dir,
                     glm::vec3& normal) {
    float best = -1.0f;
    float best_front = -1.0f;
    for (std::size_t t = 0; t < mesh.indices.size() / 3; ++t) {
        const glm::vec3& a = mesh.vertices[mesh.indices[t * 3]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[t * 3 + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[t * 3 + 2]].position;
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(dir, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-12f) continue;
        const float inv = 1.0f / det;
        const glm::vec3 s = origin - a;
        const float u = glm::dot(s, p) * inv;
        if (u < -1e-5f || u > 1.0f + 1e-5f) continue;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(dir, q) * inv;
        if (v < -1e-5f || u + v > 1.0f + 1e-5f) continue;
        const float hit = glm::dot(e2, q) * inv;
        if (hit <= 1e-4f) continue;
        const glm::vec3 n = glm::cross(e1, e2);
        if (best < 0.0f || hit < best) {
            best = hit;
            normal = n;
        }
        if (glm::dot(n, dir) < 0.0f && (best_front < 0.0f || hit < best_front)) best_front = hit;
    }
    // A ray grazing the exact edge two surfaces share can hit the back one first by a rounding
    // error; a front face within a hair of the nearest hit is what a rasteriser would draw.
    if (best_front >= 0.0f && best_front - best < 1e-3f) {
        normal = -dir;
        return best_front;
    }
    return best;
}

/**
 * @brief A deterministic, deliberately busy pad: 3x3-tile blocks at 1-4 steps (plateaus, single
 *        steps, cliffs, convex and concave corners), and 5x5 patches of every style's kind, so
 *        styles meet both across columns and down walls.
 */
toy::world::ColumnPad make_busy_pad(std::int32_t chunk_size, std::uint32_t seed) {
    using namespace toy::world;
    ColumnPad pad;
    pad.resize(chunk_size);
    auto hash = [seed](std::int32_t x, std::int32_t y) {
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u ^
                          seed * 83492791u;
        h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
        return h;
    };
    auto block = [](std::int32_t v, std::int32_t size) { return v >= 0 ? v / size : (v - size + 1) / size; };
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            TileColumn& c = pad.at(x, y);
            c.steps     = 1 + static_cast<std::int32_t>(hash(block(x, 3), block(y, 3)) % 4u);
            static const TileKind kinds[] = {TileKind::Grass, TileKind::Grass, TileKind::Stone, TileKind::Sand,
                                             TileKind::Snow};
            c.top_kind  = kinds[hash(block(x, 5) + 101, block(y, 5)) % 5u];
            c.side_kind = TileKind::Dirt;
        }
    }
    return pad;
}

} // namespace

COOPA_TEST(orientation_transforms_are_rigid) {
    using namespace toy::world;
    bool inside = true, orthogonal = true, handed = true;
    for (std::uint8_t o = 0; o < k_tile_orientation_count; ++o) {
        const glm::mat4& m = variant_transform(o);
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
            const glm::vec3 q = glm::vec3(m * glm::vec4(p, 1.0f));
            for (int k = 0; k < 3; ++k) inside = inside && q[k] > -1e-5f && q[k] < 1.0f + 1e-5f;
        }
        const glm::mat3 r(m);
        const glm::mat3 rtr = glm::transpose(r) * r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) orthogonal = orthogonal && std::abs(rtr[i][j] - (i == j ? 1.0f : 0.0f)) < 1e-5f;
        handed = handed && std::abs(glm::determinant(r) - (orientation_mirrors(o) ? -1.0f : 1.0f)) < 1e-5f;
        // Mirror first, then rotate: +X goes to -X, then turns.
        const glm::vec3 x = r * glm::vec3(1, 0, 0);
        const glm::ivec2 expected = rotate_ccw(glm::ivec2(orientation_mirrors(o) ? -1 : 1, 0), o & 3u);
        handed = handed && std::abs(x.x - expected.x) < 1e-5f && std::abs(x.y - expected.y) < 1e-5f;
    }
    expect(inside, "styled tiles: every orientation keeps the cell inside [0,1]^3");
    expect(orthogonal, "styled tiles: every orientation is a rigid motion");
    expect(handed, "styled tiles: mirrored orientations flip handedness, mirror applied before rotation");
}

COOPA_TEST(wall_ends_classify_by_neighbourhood) {
    using namespace toy::world;
    // Column (1,1) at 3 with a north wall facing the low cell (1,2) at 1. Its right end reaches
    // +X: along-wall neighbour a = (2,1), diagonal d = (2,2).
    auto classify = [](std::int32_t a, std::int32_t d, std::int32_t cell) {
        ColumnPad pad = make_styled_pad(4, 0);
        pad.at(1, 1).steps = 3;
        pad.at(1, 2).steps = 1;
        pad.at(2, 1).steps = a;
        pad.at(2, 2).steps = d;
        return classify_wall_end(pad, {1, 1}, TileFace::North, wall_end_direction(TileFace::North, true), cell);
    };
    expect(classify(1, 0, 2) == WallEnd::Convex, "wall end: a lower along-wall neighbour wraps a convex corner");
    expect(classify(3, 1, 2) == WallEnd::Continue, "wall end: a tall neighbour over a low diagonal continues");
    expect(classify(3, 3, 2) == WallEnd::Concave, "wall end: a tall neighbour and tall diagonal turn concave");
    expect(classify(2, 1, 2) == WallEnd::Convex && classify(2, 1, 1) == WallEnd::Continue,
           "wall end: classified per cell -- a short neighbour is convex above its top, straight below");

    bool placed = true;
    for (const TileFace face : k_lateral_faces) {
        for (const bool right : {true, false}) {
            const glm::mat3 r(variant_transform(wall_orientation(face, right)));
            const glm::vec3 n = r * glm::vec3(0, 1, 0);
            const glm::vec3 e = r * glm::vec3(1, 0, 0);
            const glm::ivec2 along = wall_end_direction(face, right);
            placed = placed && glm::length(n - face_normal(face)) < 1e-5f &&
                     std::abs(e.x - along.x) < 1e-5f && std::abs(e.y - along.y) < 1e-5f;
        }
    }
    expect(placed, "wall end: each half is placed on its face, reaching toward its own end");
}

COOPA_TEST(plateau_interior_merges_flat) {
    using namespace toy::world;
    const TileMeshLibrary& library = styled_library();
    const TerrainParams params = make_test_params(4);
    const ColumnPad pad = make_styled_pad(4, 3);

    ChunkMeshData mesh;
    mesh_chunk_styled(pad, library, params, mesh);
    // The quilt tripwire: a plateau interior has no rims, whatever the style -- and with the
    // interior tops merged, a uniform 4x4 plateau is a single quad.
    expect(mesh.indices.size() == 6u, "styled chunk: a flat plateau merges into one quad");
    expect(count_facing(mesh, {0, 0, 1}, [](const glm::vec3&) { return true; }) ==
               static_cast<int>(mesh.indices.size() / 3),
           "styled chunk: ...all of it flat, facing up, with no rim, wall or cap");
}

COOPA_TEST(section_caps_appear_only_at_style_changes) {
    using namespace toy::world;
    const TileMeshLibrary& library = styled_library();
    TerrainParams params = make_test_params(4);
    params.soil_depth_steps = 8; // keep the whole wall one kind: only the top style varies

    // A straight north-facing cliff along y = 2: rows y <= 1 at 2 steps, y >= 2 at 1 step.
    ColumnPad pad = make_styled_pad(4, 1);
    for (std::int32_t y = -1; y <= 1; ++y)
        for (std::int32_t x = -1; x < 5; ++x) pad.at(x, y).steps = 2;

    auto lateral_caps = [&](const ColumnPad& p) {
        ChunkMeshData mesh;
        mesh_chunk_styled(p, library, params, mesh);
        const auto anywhere = [](const glm::vec3&) { return true; };
        return count_facing(mesh, {1, 0, 0}, anywhere) + count_facing(mesh, {-1, 0, 0}, anywhere);
    };
    expect(lateral_caps(pad) == 0, "section caps: a uniform cliff needs none");

    // One column of the cliff in the rock style: its round neighbours' lips no longer match.
    pad.at(1, 1).top_kind = TileKind::Stone;
    ChunkMeshData mesh;
    mesh_chunk_styled(pad, library, params, mesh);
    const auto at_boundary = [](const glm::vec3& c) {
        return (std::abs(c.x - 1.0f) < 1e-3f || std::abs(c.x - 2.0f) < 1e-3f) && c.y > 1.0f - 1e-3f;
    };
    const int east = count_facing(mesh, {1, 0, 0}, at_boundary);
    const int west = count_facing(mesh, {-1, 0, 0}, at_boundary);
    const int all  = lateral_caps(pad);
    expect(east > 0 && west > 0, "section caps: both sides of a style change close their profiles");
    expect(all == east + west, "section caps: ...and only there");
}

/** @brief Shared by surface_has_no_holes_from_above and its soft_lod-only counterpart. */
void check_no_holes_from_above(const char* label, const toy::world::TileMeshLibrary& library,
                               bool fine = false, std::int32_t height_fine = 1, bool tilt_tops = false) {
    using namespace toy::world;
    TerrainParams params = make_test_params(16);
    params.soil_depth_steps = 1;   // soil over stone: two atlas kinds down most walls
    params.height_fine = height_fine;
    params.height_fine_tilt = tilt_tops;

    int misses = 0, backfaces = 0, rays = 0;
    std::string first_failure;
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> target(6.0f, 10.0f);
    std::uniform_real_distribution<float> tilt(0.0f, glm::radians(40.0f));
    std::uniform_real_distribution<float> azimuth(0.0f, glm::two_pi<float>());
    // STYLED_RAY_STRESS=1 casts 120k rays over 40 pads instead of 1.6k over 4 -- for after
    // changing a piece, the classifier or the cap rules.
    const std::uint32_t seeds = std::getenv("STYLED_RAY_STRESS") ? 40u : 4u;
    const int rays_per_seed   = std::getenv("STYLED_RAY_STRESS") ? 3000 : 400;
    for (std::uint32_t seed = 1; seed <= seeds; ++seed) {
        ColumnPad pad = make_busy_pad(16, seed);
        if (fine) add_sub_steps(pad, 16, seed);
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, library, params, mesh);
        for (int i = 0; i < rays_per_seed; ++i) {
            const float t = tilt(rng), a = azimuth(rng);
            const glm::vec3 dir(std::sin(t) * std::cos(a), std::sin(t) * std::sin(a), -std::cos(t));
            const glm::vec3 aim(target(rng), target(rng), 0.0f);
            const glm::vec3 origin = aim - dir * (20.0f / std::cos(t));
            glm::vec3 normal;
            const float hit = cast_chunk_ray(mesh, origin, dir, normal);
            ++rays;
            const bool miss = hit < 0.0f;
            const bool back = !miss && glm::dot(normal, dir) >= 0.0f;
            misses += miss;
            backfaces += back;
            if ((miss || back) && first_failure.empty()) {
                const glm::vec3 p = origin + dir * (miss ? 20.0f / std::cos(t) : hit);
                first_failure = std::string(miss ? "miss" : "back face") + " near (" + std::to_string(p.x) + ", " +
                                std::to_string(p.y) + ", " + std::to_string(p.z) + "), seed " + std::to_string(seed);
            }
        }
    }
    expect(misses == 0, std::string(label) + ": every ray from above hits the surface (no see-through holes)");
    expect(backfaces == 0, std::string(label) + ": every first hit is a front face (no inside showing)");
    if (!first_failure.empty()) {
        std::cerr << "         " << misses << " misses, " << backfaces << " back faces of " << rays
                  << " rays; first: " << first_failure << "\n";
    }
}

COOPA_TEST(surface_has_no_holes_from_above) { check_no_holes_from_above("styled chunk", styled_library()); }
COOPA_TEST(soft_lod_style_has_no_holes_from_above) {
    check_no_holes_from_above("soft_lod style", soft_lod_library());
}

/**
 * @brief The same hole check from GRAZING angles -- the view a player standing on the terrain
 *        actually has, where a gap in a wall, corner or fillet shows and a top-down ray never
 *        looks. Each ray aims at a point half a step under a column's top (inside solid terrain)
 *        and must meet a front face before getting there; rays whose path leaves the chunk's
 *        interior (where neighbouring chunks' geometry is missing) are not counted.
 */
/** @brief Shared by surface_has_no_holes_at_grazing_angles and its soft_lod-only counterpart. */
void check_no_holes_at_grazing_angles(const char* label, const toy::world::TileMeshLibrary& library,
                                      bool fine = false, std::int32_t height_fine = 1, bool tilt = false) {
    using namespace toy::world;
    TerrainParams params = make_test_params(32);
    params.soil_depth_steps = 1;
    params.height_fine = height_fine;
    params.height_fine_tilt = tilt;

    const std::uint32_t seeds = std::getenv("STYLED_RAY_STRESS") ? 24u : 3u;
    const int rays_per_seed   = std::getenv("STYLED_RAY_STRESS") ? 6000 : 600;
    int failures = 0, counted = 0;
    std::string first_failure;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (std::uint32_t seed = 1; seed <= seeds; ++seed) {
        ColumnPad pad = make_busy_pad(32, seed);
        if (fine) add_sub_steps(pad, 32, seed);
        ChunkMeshData mesh;
        mesh_chunk_styled(pad, library, params, mesh);
        for (int i = 0; i < rays_per_seed; ++i) {
            const int tx = 3 + static_cast<int>(unit(rng) * 26.0f), ty = 3 + static_cast<int>(unit(rng) * 26.0f);
            const glm::vec3 target(static_cast<float>(tx) + 0.2f + 0.6f * unit(rng),
                                   static_cast<float>(ty) + 0.2f + 0.6f * unit(rng),
                                   // Below the lip band: a rounded lip leaves the top few
                                   // tenths of an edge tile as air, not solid.
                                   // The column's real top, lift included: aiming at the
                                   // terrace under a lifted column is not inside its solid.
                                   // A tilted top dips toward its neighbours, so aim under the
                                   // terrace instead -- still solid, whatever the tilt did.
                                   (static_cast<float>(pad.at(tx, ty).steps) +
                                    (height_fine > 1 ? 0.0f
                                                     : static_cast<float>(pad.at(tx, ty).sub) /
                                                           static_cast<float>(k_sub_steps)) -
                                    0.55f) * params.height_step);
            const float tilt = glm::radians(30.0f + 50.0f * unit(rng)), az = glm::two_pi<float>() * unit(rng);
            const glm::vec3 dir(std::sin(tilt) * std::cos(az), std::sin(tilt) * std::sin(az), -std::cos(tilt));
            const float length = (6.0f - target.z) / std::cos(tilt);
            const glm::vec3 origin = target - dir * length;
            if (origin.x < 1.5f || origin.y < 1.5f || origin.x > 30.5f || origin.y > 30.5f) continue;
            ++counted;
            glm::vec3 normal;
            const float hit = cast_chunk_ray(mesh, origin, dir, normal);
            const bool ok = hit >= 0.0f && hit <= length + 1e-3f && glm::dot(normal, dir) < 0.0f;
            if (!ok) {
                ++failures;
                if (first_failure.empty()) {
                    first_failure = "seed " + std::to_string(seed) + " target (" + std::to_string(target.x) + ", " +
                                    std::to_string(target.y) + ", " + std::to_string(target.z) + ") dir (" +
                                    std::to_string(dir.x) + ", " + std::to_string(dir.y) + ", " + std::to_string(dir.z) +
                                    (hit < 0.0f ? ") miss" : ") back face or past target");
                }
            }
        }
    }
    expect(counted > 100, std::string(label) + ": enough grazing rays stay inside the chunk to mean something");
    expect(failures == 0, std::string(label) + ": grazing rays find no holes in walls, corners or fillets");
    if (failures > 0) std::cerr << "         " << failures << " of " << counted << "; first: " << first_failure << "\n";
}

COOPA_TEST(surface_has_no_holes_at_grazing_angles) {
    check_no_holes_at_grazing_angles("styled chunk", styled_library());
}
COOPA_TEST(soft_lod_style_has_no_holes_at_grazing_angles) {
    check_no_holes_at_grazing_angles("soft_lod style", soft_lod_library());
}

/**
 * @brief A square-edged set is what a sub-step lift needs, and the library is what says so.
 *
 * A lift translates whole stamps, so two columns lifted differently butt their pieces at an
 * offset. Square pieces have no profile depth at the tile boundary and still meet; a lipped
 * set would leave a sliver, which is why ChunkBuildJob::run() drops the lifts for one.
 */
/**
 * @brief A cut count that does not divide 256 still produces exactly that many heights a step.
 *
 * What makes neighbours merge again is not that a cut lands on a round number, but that the
 * SAME cut always produces the same stored value -- two tiles on one cut then share a height
 * exactly and need no wall between them. Five cuts of a one-metre step land 2 mm off a true
 * 20 cm, which is the price of keeping the stored byte in 1/256ths and self-describing.
 */
COOPA_TEST(a_cut_count_that_does_not_divide_256_is_still_exact) {
    using namespace toy::world;
    expect(sub_steps_for(5) == 5, "five cuts are five cuts, not four");

    std::set<std::int32_t> within_a_step;
    for (std::int64_t i = 0; i < 5; ++i) within_a_step.insert(fine_for_cut(i, 5));
    expect(within_a_step.size() == 5, "five distinct heights inside one step");

    // Every cut index maps to one value, and one step up is exactly k_sub_steps up.
    for (std::int64_t i = 0; i < 40; ++i) {
        expect(fine_for_cut(i + 5, 5) == fine_for_cut(i, 5) + k_sub_steps,
               "a cut repeats exactly one step higher");
    }
    // And quantising any height inside a cut lands on that cut's one value.
    for (std::int64_t i = 0; i < 20; ++i) {
        const double lo = static_cast<double>(i) / 5.0;
        expect(quantise_fine(lo + 1e-6, 5) == fine_for_cut(i, 5) &&
                   quantise_fine(lo + 0.19, 5) == fine_for_cut(i, 5),
               "every height within a cut quantises to the same stored value");
    }
}

COOPA_TEST(only_a_square_set_accepts_sub_step_heights) {
    using namespace toy::world;
    expect(make_tile_library({"voxel"}).sides_are_square(), "voxel: square-edged");
    expect(!make_tile_library({"round"}).sides_are_square(), "round: not square-edged");
    expect(!make_tile_library({"soft"}).sides_are_square(), "soft: not square-edged");
    // One rounded style among square ones disqualifies the whole library: a column honouring
    // its lift beside one ignoring it is the open band this guards against.
    expect(!make_tile_library({"voxel", "round"}).sides_are_square(),
           "a mixed library is not square-edged");
}

COOPA_TEST(a_sub_step_surface_has_no_holes_from_above) {
    check_no_holes_from_above("sub-step voxel chunk", voxel_library(), true);
}

COOPA_TEST(a_sub_step_surface_has_no_holes_at_grazing_angles) {
    check_no_holes_at_grazing_angles("sub-step voxel chunk", voxel_library(), true);
}

COOPA_TEST(a_tilted_surface_has_no_holes_from_above) {
    check_no_holes_from_above("tilted voxel chunk", voxel_library(), true, 64, true);
}

COOPA_TEST(a_tilted_surface_has_no_holes_at_grazing_angles) {
    check_no_holes_at_grazing_angles("tilted voxel chunk", voxel_library(), true, 64, true);
}

COOPA_TEST(a_flat_topped_fine_surface_has_no_holes_from_above) {
    check_no_holes_from_above("flat-topped fine voxel chunk", voxel_library(), true, 64);
}

COOPA_TEST(a_flat_topped_fine_surface_has_no_holes_at_grazing_angles) {
    check_no_holes_at_grazing_angles("flat-topped fine voxel chunk", voxel_library(), true, 64);
}

COOPA_TEST(tilted_meshing_is_byte_deterministic) {
    using namespace toy::world;
    TerrainParams params = make_test_params(16);
    params.soil_depth_steps = 1;
    params.height_fine = 64;
    params.height_fine_tilt = true;
    const ColumnPad pad = sub_pad(16, 9, 64);
    ChunkMeshData a, b;
    mesh_chunk_styled(pad, voxel_library(), params, a);
    mesh_chunk_styled(pad, voxel_library(), params, b);
    expect(a.vertices.size() == b.vertices.size() && a.indices.size() == b.indices.size(),
           "tilted meshing: same counts twice");
    expect(std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(a.vertices[0])) == 0,
           "tilted meshing: byte-identical vertices");
    expect(a.indices == b.indices, "tilted meshing: identical indices");
}

COOPA_TEST(sub_step_meshing_is_byte_deterministic) {
    using namespace toy::world;
    TerrainParams params = make_test_params(16);
    params.soil_depth_steps = 1;
    const ColumnPad pad = sub_pad(16, 9);
    ChunkMeshData a, b;
    mesh_chunk_styled(pad, voxel_library(), params, a);
    mesh_chunk_styled(pad, voxel_library(), params, b);
    expect(a.vertices.size() == b.vertices.size() && a.indices.size() == b.indices.size(),
           "sub-step meshing: same counts twice");
    expect(std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(a.vertices[0])) == 0,
           "sub-step meshing: byte-identical vertices -- every comparison is still an exact integer one");
    expect(a.indices == b.indices, "sub-step meshing: identical indices");
}

COOPA_TEST(styled_meshing_is_byte_deterministic) {
    using namespace toy::world;
    TerrainParams params = make_test_params(16);
    params.soil_depth_steps = 1;
    const ColumnPad pad = make_busy_pad(16, 9);
    ChunkMeshData a, b;
    mesh_chunk_styled(pad, styled_library(), params, a);
    mesh_chunk_styled(pad, styled_library(), params, b);
    const bool same = a.indices == b.indices && a.vertices.size() == b.vertices.size() &&
                      std::memcmp(a.vertices.data(), b.vertices.data(),
                                  a.vertices.size() * sizeof(a.vertices[0])) == 0;
    expect(!a.empty() && same, "styled chunk: meshing the same pad twice is byte-identical");
}
