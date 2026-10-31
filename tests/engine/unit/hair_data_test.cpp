/**
 * @file hair_data_test.cpp
 * @brief Growing a groom: area-weighted scatter, determinism, inextensible strands, and the
 *        geometry and LOD table they build.
 *
 * Every decision about where a strand goes lives in `hair_data`, which is why it is pure data and
 * pure functions with no scene and no device -- the generator, the solver and the editor all read
 * the same code, so they agree by construction rather than by three implementations matching.
 * This file is that code's only safety net: a groom is thousands of strands and nobody can see by
 * eye that a particular one is in the wrong place.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "engine/support/checks.h"

#include <toyengine/hair/hair_data.h>

COOPA_TEST_SUITE("hair_data");

namespace {

using namespace toy::hair;
using coopa::gfx::engine::data::MeshCpuData;

/**
 * @brief A flat unit square on the XY plane, normals +Z, as two triangles.
 *
 * One square metre, so `density` reads straight through as a strand count and an area-weighting
 * bug shows up as a count that is simply wrong.
 */
MeshCpuData unit_quad(float size = 1.0f) {
    MeshCpuData m;
    const float h = size;
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    m.vertices = {
        {{0.0f, 0.0f, 0.0f}, n, {0.0f, 0.0f}, {1, 0, 0, 1}},
        {{h, 0.0f, 0.0f}, n, {1.0f, 0.0f}, {1, 0, 0, 1}},
        {{h, h, 0.0f}, n, {1.0f, 1.0f}, {1, 0, 0, 1}},
        {{0.0f, h, 0.0f}, n, {0.0f, 1.0f}, {1, 0, 0, 1}},
    };
    m.indices = {0, 1, 2, 0, 2, 3};
    m.lods.push_back({0u, 6u, 0, 0.0f, {}});
    return m;
}

/**
 * @brief Two triangles of very different area sharing the plane: a small one near the origin and
 *        a large one beyond it. Area weighting must put most strands on the large one.
 */
MeshCpuData lopsided_pair() {
    MeshCpuData m;
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    m.vertices = {
        // Small triangle: area 0.005, entirely at x < 0.1.
        {{0.0f, 0.0f, 0.0f}, n, {0.0f, 0.0f}, {1, 0, 0, 1}},
        {{0.1f, 0.0f, 0.0f}, n, {0.1f, 0.0f}, {1, 0, 0, 1}},
        {{0.0f, 0.1f, 0.0f}, n, {0.0f, 0.1f}, {1, 0, 0, 1}},
        // Large triangle: area 0.5, entirely at x > 1.
        {{1.0f, 0.0f, 0.0f}, n, {1.0f, 0.0f}, {1, 0, 0, 1}},
        {{2.0f, 0.0f, 0.0f}, n, {1.0f, 0.0f}, {1, 0, 0, 1}},
        {{1.0f, 1.0f, 0.0f}, n, {1.0f, 1.0f}, {1, 0, 0, 1}},
    };
    m.indices = {0, 1, 2, 3, 4, 5};
    m.lods.push_back({0u, 6u, 0, 0.0f, {}});
    return m;
}

HairParams basic(HairShape shape = HairShape::Ribbon) {
    HairParams p;
    p.shape = shape;
    p.density = 200.0f;
    p.max_strands = 10000;
    p.length = 0.2f;
    p.length_jitter = 0.0f;
    p.segments = 4;
    p.droop = 0.0f;
    p.clump = 0.0f;
    p.curl = 0.0f;
    p.frizz_degrees = 0.0f;
    p.sanitize();
    return p;
}

/// @brief The polyline length of strand `i`: what inextensibility is measured on.
float strand_length(const StrandSet& set, std::size_t i) {
    const glm::vec3* p = set.strand(i);
    float total = 0.0f;
    for (int k = 1; k < set.points_per_strand; ++k) total += glm::length(p[k] - p[k - 1]);
    return total;
}

} // namespace

/** @brief Area, not triangle count, is what density multiplies. */
COOPA_TEST(strand_count_follows_the_scalp_area) {
    const MeshCpuData one = unit_quad(1.0f);
    const MeshCpuData four = unit_quad(2.0f);  // 2m x 2m = 4 square metres
    const ScalpMesh s1 = ScalpMesh::from_cpu_data(one);
    const ScalpMesh s4 = ScalpMesh::from_cpu_data(four);

    expect_near(scalp_area(s1), 1.0f, 1e-4f, "hair: a unit quad is one square metre");
    expect_near(scalp_area(s4), 4.0f, 1e-4f, "hair: doubling its side quadruples the area");

    HairParams p = basic();
    p.density = 200.0f;
    expect(strand_count_for(s1, p) == 200u, "hair: density x area is the strand count");
    expect(strand_count_for(s4, p) == 800u, "hair: four times the area grows four times the hair");
}

/** @brief max_strands is a ceiling, so a huge scalp cannot silently blow the budget. */
COOPA_TEST(max_strands_caps_the_count) {
    const MeshCpuData quad = unit_quad(10.0f);   // 100 square metres
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(quad);
    HairParams p = basic();
    p.density = 10000.0f;                        // would ask for a million
    p.max_strands = 5000;
    p.sanitize();
    expect(strand_count_for(s, p) == 5000u, "hair: the cap wins over density x area");
}

/** @brief Scatter is weighted by triangle AREA, not uniform over triangles. */
COOPA_TEST(scatter_is_weighted_by_triangle_area) {
    const MeshCpuData mesh = lopsided_pair();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 4000.0f;   // ~2020 strands over the 0.505 square metres
    p.sanitize();

    const StrandSet set = scatter_roots(s, p, nullptr);
    expect(set.strand_count() > 500u, "hair: the scatter produced a usable groom");

    std::size_t on_small = 0;
    for (const StrandRoot& root : set.roots) {
        if (root.position.x < 0.5f) ++on_small;
    }
    // The small triangle is 0.005 of 0.505, about 1%. Uniform-over-triangles would put 50% there.
    const float fraction = static_cast<float>(on_small) / static_cast<float>(set.strand_count());
    expect(fraction < 0.05f,
           "hair: the tiny triangle gets its ~1% share, not half -- density means the same thing "
           "however the mesh happens to be subdivided");
}

/** @brief The same seed grows the same groom; a different seed grows a different one. */
COOPA_TEST(a_groom_is_deterministic_from_its_seed) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();

    StrandSet a = scatter_roots(s, p, nullptr);
    StrandSet b = scatter_roots(s, p, nullptr);
    expect(a.strand_count() == b.strand_count(), "hair: one seed, one strand count");
    bool identical = true;
    for (std::size_t i = 0; i < a.strand_count() && identical; ++i) {
        identical = a.roots[i].position == b.roots[i].position && a.roots[i].random == b.roots[i].random;
    }
    expect(identical, "hair: and every root in exactly the same place -- a bake must be repeatable");

    p.seed = 99;
    const StrandSet c = scatter_roots(s, p, nullptr);
    bool moved = c.strand_count() != a.strand_count();
    for (std::size_t i = 0; i < std::min(a.strand_count(), c.strand_count()) && !moved; ++i) {
        moved = a.roots[i].position != c.roots[i].position;
    }
    expect(moved, "hair: a different seed is a different head of hair");
}

/** @brief Roots sit ON the scalp with the surface's normal, whatever else is randomised. */
COOPA_TEST(roots_sit_on_the_surface_facing_outward) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.frizz_degrees = 0.0f;
    p.sanitize();

    const StrandSet set = scatter_roots(s, p, nullptr);
    bool on_plane = true, facing_up = true, inside = true;
    for (const StrandRoot& root : set.roots) {
        on_plane  = on_plane && std::fabs(root.position.z) < 1e-5f;
        facing_up = facing_up && root.normal.z > 0.999f;
        inside    = inside && root.position.x >= -1e-5f && root.position.x <= 1.0f + 1e-5f &&
                              root.position.y >= -1e-5f && root.position.y <= 1.0f + 1e-5f;
    }
    expect(on_plane, "hair: every root lies on the scalp it grew from");
    expect(facing_up, "hair: with no frizz, a strand leaves along the surface normal");
    expect(inside, "hair: and no root escapes the mesh's own bounds");
}

/** @brief Jitter shortens strands, and only within the fraction it says. */
COOPA_TEST(length_jitter_stays_inside_its_bound) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.length = 0.2f;
    p.length_jitter = 0.5f;
    p.sanitize();

    const StrandSet set = scatter_roots(s, p, nullptr);
    float shortest = 1e9f, longest = 0.0f;
    for (const StrandRoot& root : set.roots) {
        shortest = std::min(shortest, root.length);
        longest  = std::max(longest, root.length);
    }
    expect(longest <= 0.2f + 1e-5f, "hair: jitter only ever shortens a strand");
    expect(shortest >= 0.1f - 1e-5f, "hair: and never past the fraction it was given");
    expect(longest - shortest > 0.02f, "hair: with jitter set, the strands are not all one length");
}

/** @brief Droop bends a strand without stretching it: hair is inextensible. */
COOPA_TEST(droop_bends_a_strand_but_never_lengthens_it) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.droop = 1.5f;              // hard enough that a naive offset would stretch visibly
    p.segments = 8;
    p.sanitize();

    StrandSet set = scatter_roots(s, p, nullptr);
    grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
    expect(set.strand_count() > 0u, "hair: the groom grew");

    bool length_held = true, bent = false, rooted = true;
    for (std::size_t i = 0; i < set.strand_count(); ++i) {
        length_held = length_held && std::fabs(strand_length(set, i) - set.roots[i].length) < 1e-3f;
        rooted      = rooted && glm::length(set.strand(i)[0] - set.roots[i].position) < 1e-5f;
        const glm::vec3 tip = set.strand(i)[set.points_per_strand - 1];
        if (tip.z < set.roots[i].position.z + 0.5f * set.roots[i].length) bent = true;
    }
    expect(length_held, "hair: a drooping strand keeps exactly the length it was authored at");
    expect(bent, "hair: and it really did fall rather than stand straight up");
    expect(rooted, "hair: while its root never leaves the scalp");
}

/** @brief Clumping pulls tips together and leaves roots where they were. */
COOPA_TEST(clumping_gathers_tips_and_leaves_roots_alone) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 2000.0f;
    p.clump = 0.9f;
    p.clump_density = 0.02f;
    p.sanitize();

    StrandSet clumped = scatter_roots(s, p, nullptr);
    grow_strands(clumped, p, glm::vec3(0.0f, 0.0f, -1.0f));

    HairParams loose = p;
    loose.clump = 0.0f;
    StrandSet separate = scatter_roots(s, loose, nullptr);
    grow_strands(separate, loose, glm::vec3(0.0f, 0.0f, -1.0f));

    // Mean distance from each tip to the nearest other tip: locks bring that down.
    const auto mean_nearest_tip = [](const StrandSet& set) {
        float total = 0.0f;
        const std::size_t n = std::min<std::size_t>(set.strand_count(), 250u);
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec3 a = set.strand(i)[set.points_per_strand - 1];
            float best = 1e9f;
            for (std::size_t j = 0; j < n; ++j) {
                if (j == i) continue;
                best = std::min(best, glm::length(set.strand(j)[set.points_per_strand - 1] - a));
            }
            total += best;
        }
        return n > 0 ? total / static_cast<float>(n) : 0.0f;
    };

    expect(mean_nearest_tip(clumped) < mean_nearest_tip(separate),
           "hair: clumping brings tips together");
    bool roots_held = true;
    for (std::size_t i = 0; i < clumped.strand_count(); ++i) {
        roots_held = roots_held && glm::length(clumped.strand(i)[0] - clumped.roots[i].position) < 1e-5f;
    }
    expect(roots_held, "hair: and never drags a root off the scalp -- roots spread, tips meet");
}

/** @brief A mask's red channel thins the groom; its green shortens it. */
COOPA_TEST(the_root_mask_thins_and_shortens) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 4000.0f;
    p.sanitize();

    const StrandSet unmasked = scatter_roots(s, p, nullptr);

    // A uniform mask: half density, half length.
    MaskImage mask;
    mask.width = 2;
    mask.height = 2;
    mask.rgba.assign(2 * 2 * 4, 0);
    for (int i = 0; i < 4; ++i) {
        mask.rgba[i * 4 + 0] = 128;  // red: density
        mask.rgba[i * 4 + 1] = 128;  // green: length
        mask.rgba[i * 4 + 2] = 255;
        mask.rgba[i * 4 + 3] = 255;
    }
    expect(mask.valid(), "hair: the test mask is a usable image");

    const StrandSet masked = scatter_roots(s, p, &mask);
    const float ratio = static_cast<float>(masked.strand_count()) /
                        static_cast<float>(std::max<std::size_t>(unmasked.strand_count(), 1u));
    expect(ratio > 0.4f && ratio < 0.6f,
           "hair: a half-strength red mask keeps about half the strands");

    float longest = 0.0f;
    for (const StrandRoot& root : masked.roots) longest = std::max(longest, root.length);
    expect(longest < 0.55f * p.length,
           "hair: and a half-strength green mask halves their length");
}

/** @brief Ribbon and tube geometry have exactly the counts their shape implies. */
COOPA_TEST(geometry_counts_match_the_shape) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);

    for (const HairShape shape : {HairShape::Ribbon, HairShape::Tube}) {
        HairParams p = basic(shape);
        p.segments = 6;
        p.cross_section = 2;
        p.sides = 5;
        p.sanitize();

        StrandSet set = scatter_roots(s, p, nullptr);
        grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
        MeshCpuData built;
        build_geometry(set, p, built);

        const std::size_t n = set.strand_count();
        expect(built.vertices.size() == n * vertices_per_strand(p),
               "hair: one strand's worth of vertices per strand, and no more");
        expect(built.indices.size() == n * indices_per_strand(p),
               "hair: likewise its indices");

        // Every index must address a real vertex -- an off-by-one in the ribbon's plane stride
        // would otherwise read whatever came next in the buffer.
        std::uint32_t highest = 0;
        for (const std::uint32_t index : built.indices) highest = std::max(highest, index);
        expect(highest < built.vertices.size(), "hair: no index points past the vertex buffer");
    }
}

/** @brief The width profile is applied along the strand, root to tip. */
COOPA_TEST(the_thickness_profile_tapers_the_strand) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic(HairShape::Ribbon);
    p.density = 1.0f;              // a single strand, so the measurement is unambiguous
    p.cross_section = 1;
    p.segments = 4;
    p.thickness = 0.02f;
    p.thickness_over_length = toy::particles::FloatCurve::linear(1.0f, 0.25f);
    p.sanitize();

    StrandSet set = scatter_roots(s, p, nullptr);
    expect(set.strand_count() == 1u, "hair: the test groom is one strand");
    grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
    MeshCpuData built;
    build_geometry(set, p, built);

    // One plane, two vertices per point: the pair's separation is the strand's width there.
    const auto width_at = [&](int point) {
        const Vertex& a = built.vertices[static_cast<std::size_t>(point) * 2u];
        const Vertex& b = built.vertices[static_cast<std::size_t>(point) * 2u + 1u];
        return glm::length(a.position - b.position);
    };
    expect_near(width_at(0), 0.02f, 1e-4f, "hair: the root is the authored thickness");
    expect_near(width_at(4), 0.02f * 0.25f, 1e-4f, "hair: and the tip is the profile's end value");
    expect(width_at(1) > width_at(3), "hair: narrowing monotonically in between");

    // uv.x must run 0..1 along the strand: the wind sway and the shadow pass both key off it.
    expect_near(built.vertices.front().uv.x, 0.0f, 1e-6f, "hair: uv.x is 0 at the root");
    expect_near(built.vertices[8].uv.x, 1.0f, 1e-6f, "hair: and 1 at the tip");
    // tangent.w must stay a real handedness: B = cross(N, T) * tangent.w in every backbone.
    for (const Vertex& v : built.vertices) {
        expect(std::fabs(std::fabs(v.tangent.w) - 1.0f) < 1e-6f,
               "hair: tangent.w is a handedness, never a smuggled payload");
    }
}

/** @brief The per-strand random reaches every vertex of its own strand and differs between them. */
COOPA_TEST(each_strand_carries_its_own_random_in_uv_y) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic(HairShape::Ribbon);
    p.density = 50.0f;
    p.sanitize();

    StrandSet set = scatter_roots(s, p, nullptr);
    grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
    MeshCpuData built;
    build_geometry(set, p, built);

    const std::size_t vps = vertices_per_strand(p);
    bool uniform_within = true;
    std::set<float> distinct;
    for (std::size_t i = 0; i < set.strand_count(); ++i) {
        const float value = built.vertices[i * vps].uv.y;
        distinct.insert(value);
        for (std::size_t k = 1; k < vps; ++k) {
            uniform_within = uniform_within && built.vertices[i * vps + k].uv.y == value;
        }
    }
    expect(uniform_within, "hair: one strand has one random -- it identifies the strand");
    expect(distinct.size() > set.strand_count() / 2,
           "hair: and strands mostly differ, or a groom would flutter as one rigid sheet");
}

/** @brief Any prefix of the LOD order is spread over the whole groom, not a bald patch. */
COOPA_TEST(the_lod_order_thins_evenly) {
    const std::vector<std::uint32_t> order = strand_lod_order(64);
    expect(order.size() == 64u, "hair: the order is a permutation of every strand");
    std::set<std::uint32_t> unique(order.begin(), order.end());
    expect(unique.size() == 64u, "hair: with no strand visited twice");

    // Half the strands must still cover both halves of the range, and quarter likewise.
    for (const std::size_t take : {32u, 16u, 8u}) {
        std::size_t low = 0;
        for (std::size_t i = 0; i < take; ++i) {
            if (order[i] < 32u) ++low;
        }
        const float balance = static_cast<float>(low) / static_cast<float>(take);
        expect(balance > 0.3f && balance < 0.7f,
               "hair: the first " + std::to_string(take) +
                   " strands still cover the whole groom -- a distant head thins, it does not go bald");
    }

    expect(strand_lod_order(0).empty(), "hair: no strands, no order");
    expect(strand_lod_order(1).size() == 1u, "hair: and a single strand is its own order");
}

/** @brief The LOD table is a run of prefixes of one index buffer, so the levels cost no memory. */
COOPA_TEST(lod_levels_are_prefixes_of_the_same_index_buffer) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 400.0f;
    p.lod_screen_sizes = glm::vec2(0.09f, 0.035f);
    p.sanitize();

    StrandSet set = scatter_roots(s, p, nullptr);
    grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
    MeshCpuData built;
    build_geometry(set, p, built);

    expect(built.lods.size() == 3u, "hair: two thresholds give two levels beyond the full groom");
    expect(built.lods[0].index_count == built.indices.size(), "hair: level 0 is the whole groom");
    for (const auto& lod : built.lods) {
        expect(lod.first_index == 0u, "hair: every level starts at the same place -- it is a prefix");
        expect(lod.index_count <= built.indices.size(), "hair: and none runs past the buffer");
    }
    expect(built.lods[1].index_count < built.lods[0].index_count, "hair: each level draws less");
    expect(built.lods[2].index_count < built.lods[1].index_count, "hair: and less again");
    expect(built.lods[1].screen_size > built.lods[2].screen_size,
           "hair: thresholds descend, so the coarser level takes over further away");
    expect_near(built.cull_screen_size, p.cull_screen_size, 1e-6f,
                "hair: the groom's own cull threshold reaches the mesh");
}

/** @brief Rewriting one strand's vertices matches what build_geometry() laid down. */
COOPA_TEST(a_strand_rewrite_reproduces_the_built_layout) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 20.0f;
    p.sanitize();

    StrandSet set = scatter_roots(s, p, nullptr);
    grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
    MeshCpuData built;
    build_geometry(set, p, built);

    // Rewriting every strand from the same points must be a no-op, which is what proves the
    // simulated path and the build path agree about the layout.
    std::vector<Vertex> rewritten = built.vertices;
    for (std::size_t i = 0; i < set.strand_count(); ++i) write_strand_vertices(set, p, i, rewritten);
    bool identical = true;
    for (std::size_t i = 0; i < rewritten.size() && identical; ++i) {
        identical = rewritten[i].position == built.vertices[i].position &&
                    rewritten[i].uv == built.vertices[i].uv;
    }
    expect(identical, "hair: a rewrite of unmoved strands changes nothing");

    // Moving one strand must touch only that strand's block.
    const std::size_t vps = vertices_per_strand(p);
    for (int k = 0; k < set.points_per_strand; ++k) set.strand(0)[k] += glm::vec3(0.0f, 0.0f, 1.0f);
    write_strand_vertices(set, p, 0, rewritten);
    expect(rewritten[0].position != built.vertices[0].position, "hair: the moved strand moved");
    bool neighbours_held = true;
    for (std::size_t i = vps; i < rewritten.size() && neighbours_held; ++i) {
        neighbours_held = rewritten[i].position == built.vertices[i].position;
    }
    expect(neighbours_held,
           "hair: and no other strand did -- a frame's upload is only the strands that moved");
}

/** @brief A guide curve sweeps the groom, without moving roots or changing strand length. */
COOPA_TEST(guides_sweep_the_groom_but_keep_roots_and_length) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 400.0f;
    p.droop = 0.0f;              // so any bend must have come from the guide
    p.guide_influence = 1.0f;
    p.guide_count = 1;
    p.sanitize();

    // A guide that leaves straight up and then bends hard along +X.
    toy::curve::CurveData curve;
    for (const glm::vec3 point : {glm::vec3(0.5f, 0.5f, 0.0f), glm::vec3(0.5f, 0.5f, 0.3f),
                                  glm::vec3(0.75f, 0.5f, 0.45f), glm::vec3(1.1f, 0.5f, 0.45f)}) {
        toy::curve::append_point(curve, point);
        curve.points.back().mode = toy::curve::HandleMode::Auto;
    }
    toy::curve::refresh_handles(curve);

    const HairGuide guide = make_hair_guide(curve, p.segments + 1);
    expect(guide.valid(), "hair: the guide resampled to a usable shape");
    expect(glm::length(guide.offsets.front()) < 1e-5f,
           "hair: a guide's first offset is zero -- its shape is measured from its own root");

    StrandSet guided = scatter_roots(s, p, nullptr);
    grow_strands(guided, p, glm::vec3(0.0f, 0.0f, -1.0f), {guide});

    StrandSet plain = scatter_roots(s, p, nullptr);
    grow_strands(plain, p, glm::vec3(0.0f, 0.0f, -1.0f));

    bool roots_held = true, length_held = true;
    float guided_x = 0.0f, plain_x = 0.0f;
    for (std::size_t i = 0; i < guided.strand_count(); ++i) {
        roots_held  = roots_held && glm::length(guided.strand(i)[0] - guided.roots[i].position) < 1e-5f;
        length_held = length_held && std::fabs(strand_length(guided, i) - guided.roots[i].length) < 1e-3f;
        guided_x += guided.strand(i)[guided.points_per_strand - 1].x - guided.roots[i].position.x;
        plain_x  += plain.strand(i)[plain.points_per_strand - 1].x - plain.roots[i].position.x;
    }
    expect(roots_held, "hair: a guide never drags a root off the scalp");
    expect(length_held, "hair: nor lengthens a strand to match its own length");
    expect(std::fabs(guided_x) > std::fabs(plain_x) + 0.1f,
           "hair: but it really does sweep the tips the way it was drawn");
}

/** @brief guide_influence blends between the procedural shape and the guided one. */
COOPA_TEST(guide_influence_blends_toward_the_drawn_shape) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 200.0f;
    p.droop = 0.0f;
    p.guide_count = 1;
    p.sanitize();

    toy::curve::CurveData curve;
    for (const glm::vec3 point : {glm::vec3(0.5f, 0.5f, 0.0f), glm::vec3(0.9f, 0.5f, 0.2f)}) {
        toy::curve::append_point(curve, point);
        curve.points.back().mode = toy::curve::HandleMode::Vector;
    }
    toy::curve::refresh_handles(curve);
    const HairGuide guide = make_hair_guide(curve, p.segments + 1);

    const auto mean_tip_x = [&](float influence) {
        HairParams q = p;
        q.guide_influence = influence;
        StrandSet set = scatter_roots(s, q, nullptr);
        grow_strands(set, q, glm::vec3(0.0f, 0.0f, -1.0f), {guide});
        float total = 0.0f;
        for (std::size_t i = 0; i < set.strand_count(); ++i) {
            total += set.strand(i)[set.points_per_strand - 1].x - set.roots[i].position.x;
        }
        return set.strand_count() > 0 ? total / static_cast<float>(set.strand_count()) : 0.0f;
    };

    const float none = mean_tip_x(0.0f);
    const float half = mean_tip_x(0.5f);
    const float full = mean_tip_x(1.0f);
    expect(std::fabs(none) < 1e-3f, "hair: influence 0 leaves the procedural shape untouched");
    expect(full > half && half > none, "hair: and the sweep grows monotonically with influence");
}

/** @brief A groom with no guides, or only unusable ones, is simply procedural. */
COOPA_TEST(guides_that_cannot_be_used_are_ignored) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.guide_influence = 1.0f;
    p.sanitize();

    toy::curve::CurveData single;
    toy::curve::append_point(single, glm::vec3(0.5f, 0.5f, 0.0f));
    expect(!make_hair_guide(single, p.segments + 1).valid(),
           "hair: one point is not a curve, so it is not a guide");

    toy::curve::CurveData degenerate;
    toy::curve::append_point(degenerate, glm::vec3(0.5f, 0.5f, 0.0f));
    toy::curve::append_point(degenerate, glm::vec3(0.5f, 0.5f, 0.0f));
    expect(!make_hair_guide(degenerate, p.segments + 1).valid(),
           "hair: and a curve with no length has no direction to replant");

    StrandSet with_junk = scatter_roots(s, p, nullptr);
    grow_strands(with_junk, p, glm::vec3(0.0f, 0.0f, -1.0f), {HairGuide{}});
    StrandSet without = scatter_roots(s, p, nullptr);
    grow_strands(without, p, glm::vec3(0.0f, 0.0f, -1.0f));
    bool same = with_junk.strand_count() == without.strand_count();
    for (std::size_t i = 0; i < with_junk.points.size() && same; ++i) {
        same = glm::length(with_junk.points[i] - without.points[i]) < 1e-6f;
    }
    expect(same, "hair: an invalid guide is ignored, not applied as a zero shape");
}

/** @brief A groom that grows nothing is empty, not malformed. */
COOPA_TEST(an_empty_groom_builds_no_geometry) {
    const MeshCpuData mesh = unit_quad();
    const ScalpMesh   s    = ScalpMesh::from_cpu_data(mesh);
    HairParams p = basic();
    p.density = 0.0f;
    p.sanitize();

    StrandSet set = scatter_roots(s, p, nullptr);
    expect(set.empty(), "hair: no density, no strands");
    grow_strands(set, p, glm::vec3(0.0f, 0.0f, -1.0f));
    MeshCpuData built;
    build_geometry(set, p, built);
    expect(built.vertices.empty() && built.indices.empty() && built.lods.empty(),
           "hair: and no geometry at all, rather than a degenerate mesh");

    const ScalpMesh nothing;
    expect(!nothing.valid(), "hair: an absent scalp is not a scalp");
    expect(strand_count_for(nothing, p) == 0u, "hair: and grows nothing");
}

/** @brief sanitize() makes a hand-edited file safe rather than trusting it. */
COOPA_TEST(sanitize_clamps_every_field_it_claims_to) {
    HairParams p;
    p.cross_section = 99;
    p.sides = 1;
    p.segments = 0;
    p.length = -4.0f;
    p.thickness = 0.0f;
    p.length_jitter = 5.0f;
    p.clump = -1.0f;
    p.lod_screen_sizes = glm::vec2(0.01f, 0.5f);   // ascending: the wrong way round
    p.sanitize();

    expect(p.cross_section == 3, "hair: three crossed planes is the most a ribbon gets");
    expect(p.sides >= 3, "hair: a tube needs at least a triangle of cross section");
    expect(p.segments >= 1, "hair: a strand needs at least one step to have a direction");
    expect(p.length > 0.0f, "hair: and a positive length to have a tangent at all");
    expect(p.thickness > 0.0f, "hair: a zero-width strand would be invisible, not cheap");
    expect(p.length_jitter <= 0.95f, "hair: jitter can never take a strand to nothing");
    expect(p.clump >= 0.0f, "hair: clumping is a fraction");
    expect(p.lod_screen_sizes.y <= p.lod_screen_sizes.x,
           "hair: LOD thresholds are forced to descend, so a level cannot shadow the one before");
}

/** @brief The hash moves on anything that changes geometry, and only on that. */
COOPA_TEST(the_params_hash_tracks_geometry_and_nothing_else) {
    HairParams a;
    a.sanitize();
    const std::uint64_t base = a.hash();
    expect(HairParams{}.hash() == base, "hair: the same params hash the same");

    HairParams b = a;
    b.density += 1.0f;
    expect(b.hash() != base, "hair: density changes the groom, so it changes the hash");
    HairParams c = a;
    c.seed = 1234;
    expect(c.hash() != base, "hair: so does the seed");
    HairParams d = a;
    d.thickness_over_length = toy::particles::FloatCurve::linear(1.0f, 0.9f);
    expect(d.hash() != base, "hair: and so does the width profile");

    // These are applied per frame instead, so moving them must not re-scatter the groom.
    HairParams e = a;
    e.lod_screen_sizes = glm::vec2(0.5f, 0.2f);
    e.cull_screen_size = 0.5f;
    e.wind_flexibility = 2.0f;
    e.wind_frequency = 10.0f;
    expect(e.hash() == base,
           "hair: LOD, culling and the wind response are re-applied per frame, so tuning them "
           "must NOT trigger a rebuild");
}
