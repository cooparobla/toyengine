/**
 * @file dent_test.cpp
 * @brief toyengine/destruct/dent.h: a blow pushes vertices inward with a radial falloff,
 *        duplicated corners at one position move together, repeated hits accumulate toward a
 *        cap, the footprint stays round under a non-uniform scale, recomputed normals reproduce
 *        the authored ones on an undented mesh, an outward-pointing blow still digs in, a miss
 *        moves nothing, and subdivision shares edge midpoints and respects its caps.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <toyengine/destruct/dent.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("dent");

using toy::destruct::apply_dent;
using toy::destruct::DentBlow;
using toy::destruct::DentState;
using toy::destruct::subdivide_for_dent;
using toy::destruct::Vertex;

namespace {

Vertex make_vertex(const glm::vec3& position, const glm::vec3& normal = glm::vec3(0.0f, 0.0f, 1.0f)) {
    Vertex v;
    v.position = position;
    v.normal = normal;
    v.uv = glm::vec2(0.0f);
    v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    return v;
}

/** @brief A welded (n+1)x(n+1) grid of vertices in the XY plane at z = 0, spanning
 *         [-size/2, size/2] on both axes, wound so face_normal points +Z. */
void flat_grid(int n, float size, std::vector<Vertex>& v, std::vector<std::uint32_t>& idx) {
    v.clear();
    idx.clear();
    const float step = size / static_cast<float>(n);
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const float x = -0.5f * size + i * step;
            const float y = -0.5f * size + j * step;
            v.push_back(make_vertex(glm::vec3(x, y, 0.0f)));
        }
    }
    const auto at = [n](int i, int j) { return static_cast<std::uint32_t>(j * (n + 1) + i); };
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    }
}

/** @brief A unit cube centred on the origin, as 12 triangles of unwelded, flat-shaded corners --
 *         every one of the 8 positions appears 3 times, once per face it touches. */
void unit_cube(std::vector<Vertex>& v, std::vector<std::uint32_t>& idx, float size = 1.0f) {
    const float h = 0.5f * size;
    const glm::vec3 corner[8] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                                 {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    const int face[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                            {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    v.clear();
    idx.clear();
    for (const auto& f : face) {
        const glm::vec3 a = corner[f[0]], b = corner[f[1]], c = corner[f[2]], d = corner[f[3]];
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        const glm::vec3 quad[4] = {a, b, c, d};
        const std::uint32_t base = static_cast<std::uint32_t>(v.size());
        for (const glm::vec3& p : quad) v.push_back(make_vertex(p, n));
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

/** @brief A regular octahedron, welded: 6 vertices, 8 faces, every edge shared by exactly 2. */
void octahedron(std::vector<Vertex>& v, std::vector<std::uint32_t>& idx) {
    v.clear();
    idx.clear();
    v.push_back(make_vertex({1, 0, 0}));
    v.push_back(make_vertex({-1, 0, 0}));
    v.push_back(make_vertex({0, 1, 0}));
    v.push_back(make_vertex({0, -1, 0}));
    v.push_back(make_vertex({0, 0, 1}));
    v.push_back(make_vertex({0, 0, -1}));
    idx = {4, 0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 5, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3};
}

} // namespace

COOPA_TEST(a_dent_pushes_vertices_inward_and_leaves_the_rim_untouched) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    flat_grid(6, 2.0f, v, idx); // a 7x7 grid spanning [-1, 1]

    DentState state;
    state.build(v, idx);
    require(!state.empty(), "dent: the grid produced at least one unique position");

    const glm::mat4 world(1.0f);
    DentBlow blow;
    blow.point = glm::vec3(0.0f, 0.0f, 0.0f);
    blow.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    blow.radius = 0.4f;
    blow.depth = 0.2f;
    blow.force_inward = false;

    const std::size_t touched = apply_dent(state, v, idx, blow, world, world);
    expect(touched > 0, "dent: the blow touched at least one vertex near its centre");

    bool centre_moved = false, rim_untouched = true;
    for (const Vertex& vert : v) {
        const float r = glm::length(glm::vec2(vert.position.x, vert.position.y));
        if (r < 0.1f && vert.position.z < -1e-4f) centre_moved = true;
        if (r > 0.9f && std::abs(vert.position.z) > 1e-5f) rim_untouched = false;
    }
    expect(centre_moved, "dent: the vertex under the blow was pushed inward (negative Z)");
    expect(rim_untouched, "dent: a rim vertex well outside the radius was not moved");
}

COOPA_TEST(duplicated_corners_at_one_position_move_together_so_the_mesh_does_not_tear) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx);

    const glm::vec3 target_corner(0.5f, 0.5f, 0.5f);
    std::vector<std::size_t> duplicates;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (glm::distance(v[i].position, target_corner) < 1e-5f) duplicates.push_back(i);
    }
    require(duplicates.size() == 3, "dent: the (+,+,+) corner of a hard-edged cube is 3 vertices");

    DentState state;
    state.build(v, idx);

    const glm::mat4 world(1.0f);
    DentBlow blow;
    blow.point = target_corner;
    blow.direction = glm::normalize(-target_corner); // toward the cube's centre
    blow.radius = 0.3f;
    blow.depth = 0.1f;
    blow.force_inward = false;

    const std::size_t touched = apply_dent(state, v, idx, blow, world, world);
    expect(touched > 0, "dent: the blow touched the corner's shared position");

    const glm::vec3 moved_to = v[duplicates[0]].position;
    expect(glm::distance(moved_to, target_corner) > 1e-4f, "dent: the corner actually moved");
    for (std::size_t i : duplicates) {
        expect(glm::distance(v[i].position, moved_to) < 1e-5f,
              "dent: every duplicate at that position landed at the exact same new position");
    }
}

COOPA_TEST(repeated_dents_on_one_spot_deepen_toward_max_offset_and_then_stop) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    flat_grid(4, 2.0f, v, idx);

    DentState state;
    state.build(v, idx);

    const glm::mat4 world(1.0f);
    DentBlow blow;
    blow.point = glm::vec3(0.0f);
    blow.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    blow.radius = 0.6f;
    blow.depth = 0.05f;
    blow.max_offset = 0.12f;
    blow.force_inward = false;

    float previous = 0.0f;
    for (int i = 0; i < 20; ++i) {
        apply_dent(state, v, idx, blow, world, world);
        const float now = state.max_offset();
        expect(now >= previous - 1e-6f, "dent: accumulated depth never decreases across repeated hits");
        expect(now <= blow.max_offset + 1e-5f, "dent: ...and never exceeds the cap");
        previous = now;
    }
    expect(previous > blow.max_offset - 1e-3f, "dent: enough repeated hits reach the cap");
}

COOPA_TEST(a_dent_under_a_non_uniform_scale_stays_round_in_world_space) {
    // Two points at DIFFERENT object-space distances from the origin that land at the SAME
    // world-space distance once a non-uniform scale is applied.
    std::vector<Vertex> v = {make_vertex({1.0f, 0.0f, 0.0f}), make_vertex({0.0f, 2.0f, 0.0f}),
                             make_vertex({5.0f, 5.0f, 5.0f})};
    std::vector<std::uint32_t> idx = {0, 1, 2};

    DentState state;
    state.build(v, idx);

    const glm::mat4 world = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    const glm::mat4 inv_world = glm::inverse(world);

    DentBlow blow;
    blow.point = glm::vec3(0.0f);
    blow.direction = glm::vec3(0.0f, 0.0f, 1.0f);
    blow.radius = 2.5f; // covers both world-space points (both at world distance 2.0)
    blow.depth = 0.1f;
    blow.force_inward = false;

    apply_dent(state, v, idx, blow, world, inv_world);

    expect_near(glm::length(state.offset[0]), glm::length(state.offset[1]), 1e-4f,
               "dent: object-space points (1,0,0) and (0,2,0) -- unequal object-space distance, "
               "equal world-space distance under scale(2,1,1) -- get an equal push");
}

COOPA_TEST(recomputed_normals_reproduce_the_authored_normals_on_an_undented_mesh) {
    // Hard-edged case: a cube corner far from the blow keeps its single-face flat normal.
    {
        std::vector<Vertex> v;
        std::vector<std::uint32_t> idx;
        unit_cube(v, idx);
        const glm::vec3 far_corner(-0.5f, -0.5f, -0.5f);
        std::size_t far_index = v.size();
        glm::vec3 far_original_normal(0.0f);
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (glm::distance(v[i].position, far_corner) < 1e-5f) {
                far_index = i;
                far_original_normal = v[i].normal;
                break;
            }
        }
        require(far_index < v.size(), "dent: found the far corner");

        DentState state;
        state.build(v, idx);
        const glm::mat4 world(1.0f);
        DentBlow blow;
        blow.point = glm::vec3(0.5f, 0.5f, 0.5f); // the opposite corner
        blow.direction = glm::vec3(-1.0f, -1.0f, -1.0f);
        blow.radius = 0.2f;
        blow.depth = 0.05f;
        blow.force_inward = false;
        const std::size_t touched = apply_dent(state, v, idx, blow, world, world);
        require(touched > 0, "dent: the blow touched the near corner, triggering a recompute");

        expect(glm::distance(v[far_index].normal, far_original_normal) < 1e-4f,
              "dent: the untouched far corner's flat normal is unchanged by the full-mesh recompute");
    }

    // Smooth case: two flat hexagonal fans, far apart and topologically disjoint. A fan's hub
    // vertex is shared by all 6 of its triangles, so by symmetry its area-weighted normal is
    // exactly +Z. The blow lands in fan 1 (perturbing its hub's own normal, which is not what
    // this checks); fan 2's hub shares no triangle with anything that moved, so its normal must
    // reproduce exactly -- the same "untouched region is unaffected" claim the cube case makes,
    // now for a genuinely multi-triangle-averaged (smooth) vertex rather than a flat-shaded one.
    {
        const auto add_fan = [](std::vector<Vertex>& v, std::vector<std::uint32_t>& idx, const glm::vec3& centre) {
            const std::uint32_t hub = static_cast<std::uint32_t>(v.size());
            v.push_back(make_vertex(centre));
            for (int k = 0; k < 6; ++k) {
                const float a = k * (2.0f * 3.14159265f / 6.0f);
                v.push_back(make_vertex(centre + glm::vec3(std::cos(a), std::sin(a), 0.0f)));
            }
            for (int k = 0; k < 6; ++k) {
                const std::uint32_t a = hub + 1 + static_cast<std::uint32_t>(k);
                const std::uint32_t b = hub + 1 + static_cast<std::uint32_t>((k + 1) % 6);
                idx.insert(idx.end(), {hub, a, b});
            }
            return hub;
        };

        std::vector<Vertex> v;
        std::vector<std::uint32_t> idx;
        const std::uint32_t hub1 = add_fan(v, idx, glm::vec3(0.0f));
        const std::uint32_t hub2 = add_fan(v, idx, glm::vec3(20.0f, 0.0f, 0.0f)); // far from fan 1

        DentState state;
        state.build(v, idx);
        const glm::mat4 world(1.0f);
        DentBlow blow;
        blow.point = v[hub1 + 1].position; // fan 1's own rim, nowhere near fan 2
        blow.direction = glm::vec3(0.0f, 0.0f, -1.0f);
        blow.radius = 0.3f;
        blow.depth = 0.05f;
        blow.force_inward = false;
        const std::size_t touched = apply_dent(state, v, idx, blow, world, world);
        require(touched > 0, "dent: the blow touched fan 1's rim, triggering a full-mesh recompute");

        expect_near(v[hub2].normal.z, 1.0f, 1e-4f,
                   "dent: fan 2's smooth, 6-triangle-averaged hub normal is unchanged -- nothing "
                   "it shares a triangle with moved");
    }
}

COOPA_TEST(a_blow_whose_direction_points_out_of_the_surface_still_dents_inward) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx); // centred on the origin, half-size 0.5

    std::vector<glm::vec3> original(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) original[i] = v[i].position;

    DentState state;
    state.build(v, idx);
    const glm::mat4 world(1.0f); // object space == world space, centre at the origin

    DentBlow blow;
    blow.point = glm::vec3(0.5f, 0.0f, 0.0f);     // the +X face
    blow.direction = glm::vec3(1.0f, 0.0f, 0.0f); // WRONG on purpose: points further outward
    // The cube has no face-centre vertex, only corners -- the nearest ones to (0.5, 0, 0) are
    // its own face's, at distance sqrt(0.5) =~ 0.707; the -X face's are at sqrt(1.5) =~ 1.225.
    // 0.8 reaches the first and not the second.
    blow.radius = 0.8f;
    blow.depth = 0.1f;
    blow.force_inward = true; // default, spelled out for clarity

    const std::size_t touched = apply_dent(state, v, idx, blow, world, world);
    require(touched > 0, "dent: the blow touched the +X face");

    bool any_moved_inward = false, any_moved_outward = false;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (original[i].x < 0.4f) continue; // only the +X face's own vertices
        if (v[i].position.x < original[i].x - 1e-4f) any_moved_inward = true;
        if (v[i].position.x > original[i].x + 1e-4f) any_moved_outward = true;
    }
    expect(any_moved_inward, "dent: a +X face vertex moved toward -X (into the cube)");
    expect(!any_moved_outward, "dent: force_inward flipped the outward-pointing direction, so "
                               "nothing on the +X face moved further out");
}

COOPA_TEST(a_blow_outside_the_footprint_moves_nothing) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    flat_grid(4, 2.0f, v, idx);
    std::vector<glm::vec3> original(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) original[i] = v[i].position;

    DentState state;
    state.build(v, idx);
    const glm::mat4 world(1.0f);

    DentBlow blow;
    blow.point = glm::vec3(100.0f, 100.0f, 0.0f); // far outside the grid
    blow.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    blow.radius = 0.5f;
    blow.depth = 0.2f;

    const std::size_t touched = apply_dent(state, v, idx, blow, world, world);
    expect(touched == 0, "dent: a blow with no vertex inside its radius touches none");
    for (std::size_t i = 0; i < v.size(); ++i) {
        expect(glm::distance(v[i].position, original[i]) < 1e-6f,
              "dent: and no vertex position changed");
    }
    expect(state.max_offset() == 0.0f, "dent: ...nor did any accumulated offset");
}

COOPA_TEST(subdivision_shares_edge_midpoints_so_the_surface_stays_closed) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    octahedron(v, idx);
    require(v.size() == 6 && idx.size() == 24, "dent: the octahedron fixture is 6 verts, 8 faces");

    // Force exactly one level: an impossibly small target, capped at 1.
    const int levels = subdivide_for_dent(v, idx, /*target_edge_world=*/1e-6f, /*world_scale=*/1.0f,
                                          /*max_levels=*/1);
    expect(levels == 1, "dent: subdivision applied exactly the one level it was capped to");
    expect(v.size() == 18, "dent: 6 original + 12 edge midpoints = 18 vertices");
    expect(idx.size() == 96, "dent: 8 faces x 4 = 32 triangles = 96 indices");

    // No edge produced two different midpoints: re-welding by position finds the same 18.
    DentState state;
    state.build(v, idx);
    expect(state.rest.size() == 18, "dent: every shared edge's midpoint is one position, not two "
                                    "-- the subdivided surface has no cracks");

    // A UV seam: two triangles that share an edge by POSITION only, each with its own copy of
    // the edge's two endpoints (as a real UV seam in an asset mesh would). Both triangles'
    // independently-computed midpoints for that shared edge must still land on the exact same
    // position, or the seam would pull apart under subdivision.
    std::vector<Vertex> seam_v = {make_vertex({0, 0, 0}), make_vertex({1, 0, 0}), make_vertex({0, 1, 0}),
                                  make_vertex({0, 0, 0}), make_vertex({1, 0, 0}), make_vertex({0, -1, 0})};
    std::vector<std::uint32_t> seam_idx = {0, 1, 2, 3, 4, 5};
    const int seam_levels =
        subdivide_for_dent(seam_v, seam_idx, 1e-6f, 1.0f, /*max_levels=*/1, /*max_vertices=*/1000);
    expect(seam_levels == 1, "dent: the seam fixture also subdivided once");
    // Triangle 1's a-b edge midpoint is the first new vertex (index 6); triangle 2's own copy of
    // that same edge is the next one the loop creates (index 9, after tri1's 3 midpoints).
    require(seam_v.size() >= 10, "dent: the seam fixture produced at least two midpoint batches");
    expect(seam_v[6].position == seam_v[9].position,
          "dent: the two independently-computed midpoints of the same shared edge are bit-identical");
}

COOPA_TEST(subdivision_stops_at_the_level_and_vertex_caps) {
    {
        std::vector<Vertex> v;
        std::vector<std::uint32_t> idx;
        octahedron(v, idx);
        const int levels = subdivide_for_dent(v, idx, 1e-6f, 1.0f, /*max_levels=*/2);
        expect(levels == 2, "dent: the level cap stopped it at 2, not the (unreachable) target");
    }
    {
        // One level needs 6 + 12 = 18 vertices; a cap of 10 refuses even the first level.
        std::vector<Vertex> v;
        std::vector<std::uint32_t> idx;
        octahedron(v, idx);
        const int levels = subdivide_for_dent(v, idx, 1e-6f, 1.0f, /*max_levels=*/10, /*max_vertices=*/10);
        expect(levels == 0, "dent: the vertex cap refused the first level");
        expect(v.size() == 6, "dent: ...and left the mesh untouched");
    }
}

COOPA_TEST(a_mesh_already_finer_than_the_target_is_left_alone) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    octahedron(v, idx);
    const std::size_t original_vertex_count = v.size();
    const std::size_t original_index_count = idx.size();

    const int levels = subdivide_for_dent(v, idx, /*target_edge_world=*/100.0f, /*world_scale=*/1.0f);
    expect(levels == 0, "dent: a mesh already finer than the target is left alone");
    expect(v.size() == original_vertex_count, "dent: vertex count is unchanged");
    expect(idx.size() == original_index_count, "dent: index count is unchanged");
}
