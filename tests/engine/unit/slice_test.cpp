/**
 * @file slice_test.cpp
 * @brief Plane slicing (toyengine/destruct/slice.h): a cut cube closes into two halves that sum
 *        back to the source, each half is centred on its own origin (so it can be a rigid
 *        body), the cap is told apart from the source surface, a miss reports false, and a
 *        multi-ring cut (a torus through its hole) still closes both pieces.
 *
 * Volume is the measure throughout, for the same reason fracture_test uses it: it is the
 * property slicing has to preserve, and a leaking cap or a mesh that kept the wrong side shows
 * up there first.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <toyengine/destruct/slice.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("slice");

using toy::destruct::SlicePiece;
using toy::destruct::SliceSettings;
using toy::destruct::Vertex;

namespace {

/** @brief A cube centred on the origin, as 12 triangles of unwelded corners. */
void unit_cube(std::vector<Vertex>& v, std::vector<std::uint32_t>& i, float size) {
    const float h = 0.5f * size;
    const glm::vec3 corner[8] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                                 {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    const int face[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                            {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    v.clear();
    i.clear();
    for (const auto& f : face) {
        const glm::vec3 a = corner[f[0]], b = corner[f[1]], c = corner[f[2]], d = corner[f[3]];
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        const glm::vec3 quad[4] = {a, b, c, d};
        const std::uint32_t base = static_cast<std::uint32_t>(v.size());
        for (const glm::vec3& p : quad) {
            Vertex vert;
            vert.position = p;
            vert.normal = n;
            vert.uv = glm::vec2(0.0f);
            vert.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            v.push_back(vert);
        }
        i.insert(i.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

/** @brief A closed torus (genus 1), the central ring in the XY plane. */
void torus_mesh(std::vector<Vertex>& v, std::vector<std::uint32_t>& idx, float major_radius,
                float minor_radius, int major_segments, int minor_segments) {
    v.clear();
    idx.clear();
    v.reserve(static_cast<std::size_t>(major_segments) * static_cast<std::size_t>(minor_segments));
    for (int i = 0; i < major_segments; ++i) {
        const float u = 2.0f * glm::pi<float>() * static_cast<float>(i) / static_cast<float>(major_segments);
        const glm::vec3 radial(std::cos(u), std::sin(u), 0.0f);
        const glm::vec3 centre = major_radius * radial;
        for (int j = 0; j < minor_segments; ++j) {
            const float w = 2.0f * glm::pi<float>() * static_cast<float>(j) / static_cast<float>(minor_segments);
            const glm::vec3 normal = std::cos(w) * radial + std::sin(w) * glm::vec3(0.0f, 0.0f, 1.0f);
            Vertex vert;
            vert.position = centre + minor_radius * normal;
            vert.normal = normal;
            vert.uv = glm::vec2(static_cast<float>(i) / static_cast<float>(major_segments),
                                static_cast<float>(j) / static_cast<float>(minor_segments));
            vert.tangent = glm::vec4(-std::sin(u), std::cos(u), 0.0f, 1.0f);
            v.push_back(vert);
        }
    }
    const auto at = [&](int i, int j) {
        const int mi = ((i % major_segments) + major_segments) % major_segments;
        const int mj = ((j % minor_segments) + minor_segments) % minor_segments;
        return static_cast<std::uint32_t>(mi * minor_segments + mj);
    };
    for (int i = 0; i < major_segments; ++i) {
        for (int j = 0; j < minor_segments; ++j) {
            const std::uint32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            idx.insert(idx.end(), {a, b, c, a, c, d});
        }
    }
}

/** @brief Signed volume of a closed mesh (the divergence theorem over its faces). */
double volume_of(const std::vector<Vertex>& v, const std::vector<std::uint32_t>& idx) {
    double total = 0.0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const glm::dvec3 a(v[idx[t]].position), b(v[idx[t + 1]].position), c(v[idx[t + 2]].position);
        total += glm::dot(a, glm::cross(b, c)) / 6.0;
    }
    return std::abs(total);
}

} // namespace

/** @brief A plane through the middle of a cube gives two closed halves summing back to it. */
COOPA_TEST(slicing_a_cube_through_its_middle_gives_two_closed_halves) {
    std::vector<Vertex> v, i_unused;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx, 2.0f); // volume 8

    SlicePiece neg, pos;
    const bool ok = toy::destruct::slice_mesh(v, idx, glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, SliceSettings{}, neg, pos);
    expect(ok, "slice: a plane through the middle cuts the cube");
    expect_near(static_cast<float>(volume_of(neg.vertices, neg.indices)), 4.0f, 1e-3f, "slice: negative half is 4.0");
    expect_near(static_cast<float>(volume_of(pos.vertices, pos.indices)), 4.0f, 1e-3f, "slice: positive half is 4.0");
    expect_near(neg.volume, 4.0f, 1e-3f, "slice: SlicePiece::volume matches the negative half");
    expect_near(pos.volume, 4.0f, 1e-3f, "slice: SlicePiece::volume matches the positive half");
    expect_near(neg.volume + pos.volume, 8.0f, 1e-3f, "slice: the two halves sum back to the source");
}

/** @brief A plane that misses the mesh entirely leaves the caller's object alone. */
COOPA_TEST(a_plane_that_misses_returns_false_and_leaves_outputs_empty) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx, 2.0f);

    SlicePiece neg, pos;
    neg.volume = 42.0f; // sentinel: slice_mesh must reset outputs even on failure
    const bool ok = toy::destruct::slice_mesh(v, idx, glm::vec3(1.0f, 0.0f, 0.0f), 10.0f, SliceSettings{}, neg, pos);
    expect(!ok, "slice: a plane entirely outside the mesh misses");
    expect(neg.vertices.empty() && neg.indices.empty(), "slice: the negative output is left empty");
    expect(pos.vertices.empty() && pos.indices.empty(), "slice: the positive output is left empty");
}

/** @brief Each half is re-centred on its own principal frame, so it can be dropped straight
 *        onto a rigid body without an extra recentring step. */
COOPA_TEST(each_half_is_centred_on_its_own_origin) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx, 2.0f);

    SlicePiece neg, pos;
    const bool ok = toy::destruct::slice_mesh(v, idx, glm::vec3(0.0f, 1.0f, 0.0f), 0.3f, SliceSettings{}, neg, pos);
    expect(ok, "slice: an off-centre plane still cuts the cube");
    for (const SlicePiece* piece : {&neg, &pos}) {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Vertex& vert : piece->vertices) {
            lo = glm::min(lo, vert.position);
            hi = glm::max(hi, vert.position);
        }
        expect(glm::length(lo + hi) < 1e-3f, "slice: a half's own bounds are centred on its origin");
    }
}

/** @brief Triangles from cap_first_index on are the new cut face, not the source surface. */
COOPA_TEST(cap_first_index_splits_the_source_triangles_from_the_cap) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx, 2.0f);

    SlicePiece neg, pos;
    const bool ok = toy::destruct::slice_mesh(v, idx, glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, SliceSettings{}, neg, pos);
    expect(ok, "slice: the cube cuts");
    for (const SlicePiece* piece : {&neg, &pos}) {
        expect(piece->cap_first_index > 0, "slice: there is at least one source-surface triangle before the cap");
        expect(piece->cap_first_index < piece->indices.size(), "slice: there is at least one cap triangle");
        expect((piece->indices.size() - piece->cap_first_index) % 3 == 0,
               "slice: the cap region is a whole number of triangles");
    }
}

/** @brief A plane through a torus's hole crosses the tube twice per side -- two disjoint rings
 *        in the cut plane, the "multi-ring" case slice_mesh inherits from clip_mesh. Both
 *        pieces must still close with a meaningful, positive volume. */
COOPA_TEST(slicing_a_torus_through_its_hole_gives_two_closed_pieces) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    const float major_radius = 1.0f, minor_radius = 0.35f;
    torus_mesh(v, idx, major_radius, minor_radius, 32, 20);
    const float analytic_volume = 2.0f * glm::pi<float>() * glm::pi<float>() * major_radius * minor_radius * minor_radius;
    expect_near(static_cast<float>(volume_of(v, idx)), analytic_volume, analytic_volume * 0.05f,
               "slice: the torus mesh itself measures close to the analytic volume");

    SlicePiece neg, pos;
    const bool ok = toy::destruct::slice_mesh(v, idx, glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, SliceSettings{}, neg, pos);
    expect(ok, "slice: a plane through the hole cuts the torus into two pieces");
    expect(neg.volume > 0.1f * analytic_volume, "slice: the negative piece has a meaningful positive volume");
    expect(pos.volume > 0.1f * analytic_volume, "slice: the positive piece has a meaningful positive volume");
}

/** @brief No RNG anywhere in the path: the same cut on the same mesh is bit-identical. */
COOPA_TEST(the_same_cut_twice_is_byte_identical) {
    std::vector<Vertex> v;
    std::vector<std::uint32_t> idx;
    unit_cube(v, idx, 2.0f);

    SlicePiece neg_a, pos_a, neg_b, pos_b;
    toy::destruct::slice_mesh(v, idx, glm::vec3(0.3f, 0.5f, 0.2f), 0.1f, SliceSettings{}, neg_a, pos_a);
    toy::destruct::slice_mesh(v, idx, glm::vec3(0.3f, 0.5f, 0.2f), 0.1f, SliceSettings{}, neg_b, pos_b);

    expect(neg_a.vertices.size() == neg_b.vertices.size(), "slice: repeated cuts give the same vertex count");
    bool identical = neg_a.indices == neg_b.indices;
    for (std::size_t n = 0; identical && n < neg_a.vertices.size(); ++n) {
        identical = neg_a.vertices[n].position == neg_b.vertices[n].position;
    }
    expect(identical, "slice: repeated cuts are bit-identical");
}
