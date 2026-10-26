/**
 * @file instance_brush_test.cpp
 * @brief The instance brush's pure functions (editor/instance/instance_brush.h): where a dab
 *        offers candidates, how a clump is distributed, what grid snapping lands on, what one
 *        instance ends up looking like, and what an undo patch restores.
 *
 * No editor, no scene, no device -- value in, value out over an InstancePrototype, exactly as
 * editor/terrain/'s brushes are tested.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "editor/instance/instance_brush.h"

COOPA_TEST_SUITE("instance_brush");

using coopa::test::expect;
using coopa::test::expect_near;
using coopa::test::require;
using toy::editor::brush_candidates;
using toy::editor::cluster_candidates;
using toy::editor::cluster_prototype;
using toy::editor::InstanceBrushSettings;
using toy::editor::InstanceTool;
using toy::editor::make_record;
using toy::editor::snap_to_grid;
using toy::editor::SurfaceSample;
using toy::instance::InstancePrototype;

namespace {

InstancePrototype pine() {
    InstancePrototype p;
    p.object = "props/pine";
    p.scale_range = glm::vec2(1.0f, 2.0f);
    p.yaw_range = glm::vec2(0.0f, 360.0f);
    p.bounds_min = glm::vec3(-1.0f, -1.0f, 0.0f);
    p.bounds_max = glm::vec3(1.0f, 1.0f, 8.0f);
    return p;
}

SurfaceSample flat(glm::vec2 xy) {
    SurfaceSample s;
    s.position = glm::vec3(xy, 0.0f);
    s.normal = glm::vec3(0.0f, 0.0f, 1.0f);
    return s;
}

} // namespace

COOPA_TEST(candidate_count_tracks_density_and_radius) {
    InstanceBrushSettings s;
    s.radius = 5.0f;
    s.falloff = 0.0f;     // a hard-edged disc, so the count is the area times the density
    s.density = 1.0f;
    const std::size_t at_1 = brush_candidates(s, glm::vec2(0.0f), 1234u).size();
    // pi r^2 = 78.5 at r = 5; the jittered grid is approximate, so allow a generous band.
    expect(at_1 > 50 && at_1 < 110,
           "instance_brush: at 1 per m2 over a 5 m disc a dab offers roughly the disc's area "
           "in candidates (about 79)");

    s.density = 4.0f;
    const std::size_t at_4 = brush_candidates(s, glm::vec2(0.0f), 1234u).size();
    expect(at_4 > at_1 * 3,
           "instance_brush: quadrupling the density roughly quadruples the candidates -- "
           "density is per square metre, not per dab");

    s.density = 1.0f;
    s.radius = 10.0f;
    const std::size_t big = brush_candidates(s, glm::vec2(0.0f), 1234u).size();
    expect(big > at_1 * 3,
           "instance_brush: doubling the radius quadruples the area, and so the candidates");
}

COOPA_TEST(every_candidate_lands_inside_the_brush) {
    InstanceBrushSettings s;
    s.radius = 3.0f;
    s.density = 3.0f;
    s.falloff = 0.0f;
    const glm::vec2 centre(12.0f, -7.0f);
    const auto points = brush_candidates(s, centre, 99u);
    require(!points.empty(), "instance_brush: the dab offers something to place");
    for (glm::vec2 p : points) {
        expect(glm::length(p - centre) < s.radius + 1e-4f,
               "instance_brush: no candidate falls outside the brush circle");
    }
}

COOPA_TEST(falloff_thins_the_rim_rather_than_cutting_it) {
    InstanceBrushSettings s;
    s.radius = 6.0f;
    s.density = 4.0f;
    s.falloff = 0.8f;
    const glm::vec2 centre(0.0f);
    const auto points = brush_candidates(s, centre, 7u);
    require(points.size() > 40, "instance_brush: enough candidates to measure a distribution");

    // Density per unit area in the inner third against the outer third.
    int inner = 0, outer = 0;
    for (glm::vec2 p : points) {
        const float d = glm::length(p - centre) / s.radius;
        if (d < 0.33f) ++inner;
        else if (d > 0.66f) ++outer;
    }
    const float inner_area = 3.14159f * 0.33f * 0.33f;
    const float outer_area = 3.14159f * (1.0f - 0.66f * 0.66f);
    const float inner_rate = static_cast<float>(inner) / inner_area;
    const float outer_rate = static_cast<float>(outer) / outer_area;
    expect(inner_rate > outer_rate * 1.5f,
           "instance_brush: falloff is a keep probability, so the rim comes out visibly "
           "sparser than the core instead of ending in a hard ring of props");
    expect(outer > 0,
           "instance_brush: ...but not empty -- a hard cut is what falloff exists to avoid");
}

COOPA_TEST(the_same_seed_gives_the_same_dab) {
    InstanceBrushSettings s;
    s.radius = 4.0f;
    s.density = 2.0f;
    const auto a = brush_candidates(s, glm::vec2(3.0f, 4.0f), 555u);
    const auto b = brush_candidates(s, glm::vec2(3.0f, 4.0f), 555u);
    const auto c = brush_candidates(s, glm::vec2(3.0f, 4.0f), 556u);
    expect(a == b,
           "instance_brush: candidates are hashed from (seed, cell), so a dab is reproducible "
           "whatever order it is consumed in");
    expect(a != c, "instance_brush: a different seed gives a different dab");
}

COOPA_TEST(snapping_lands_exactly_on_cell_centres) {
    InstanceBrushSettings s;
    s.snap = true;
    s.grid_cell = 2.0f;
    s.grid_origin = glm::vec2(0.0f);
    expect_near(snap_to_grid(s, glm::vec2(0.1f, 0.1f)).x, 1.0f, 1e-5f,
                "instance_brush: a point just inside the first cell snaps to its centre (1 m)");
    expect_near(snap_to_grid(s, glm::vec2(1.9f, 0.1f)).x, 1.0f, 1e-5f,
                "instance_brush: so does a point at the far side of the same cell");
    expect_near(snap_to_grid(s, glm::vec2(2.1f, 0.1f)).x, 3.0f, 1e-5f,
                "instance_brush: the next cell over snaps to 3 m");
    expect_near(snap_to_grid(s, glm::vec2(-0.1f, 0.0f)).x, -1.0f, 1e-5f,
                "instance_brush: snapping floors, so it is symmetric about the origin");

    s.grid_origin = glm::vec2(0.5f, 0.0f);
    expect_near(snap_to_grid(s, glm::vec2(0.6f, 0.0f)).x, 1.5f, 1e-5f,
                "instance_brush: the grid origin shifts the phase, so a kit can line up with "
                "something already placed");

    // Snapped painting must walk whole cells: candidates landing several to a cell would all but
    // one be thrown away by one-per-cell, which reads as a thin, patchy brush.
    s.grid_origin = glm::vec2(0.0f);
    s.radius = 5.0f;
    const auto points = brush_candidates(s, glm::vec2(0.0f), 11u);
    for (glm::vec2 p : points) {
        expect_near(glm::length(p - snap_to_grid(s, p)), 0.0f, 1e-4f,
                    "instance_brush: with snap on, every candidate is already a cell centre");
    }
}

COOPA_TEST(a_clump_respects_its_count_range_and_centre_bias) {
    InstanceBrushSettings s;
    s.cluster_count = glm::ivec2(4, 9);
    s.cluster_spread = 3.0f;
    for (std::uint32_t seed = 1; seed < 40; ++seed) {
        const auto points = cluster_candidates(s, glm::vec2(0.0f), seed);
        expect(points.size() >= 4 && points.size() <= 9,
               "instance_brush: a clump holds between Count Min and Count Max instances");
        for (glm::vec2 p : points) {
            expect(glm::length(p) <= s.cluster_spread + 1e-4f,
                   "instance_brush: no member of a clump falls outside its spread");
        }
    }

    // Mean radius: even over the disc is 2/3 of the spread; biased to the centre is 1/2.
    auto mean_radius = [&](float bias) {
        s.cluster_bias = bias;
        s.cluster_count = glm::ivec2(40, 40);
        float total = 0.0f;
        int n = 0;
        for (std::uint32_t seed = 1; seed < 30; ++seed) {
            for (glm::vec2 p : cluster_candidates(s, glm::vec2(0.0f), seed)) {
                total += glm::length(p);
                ++n;
            }
        }
        return n > 0 ? total / static_cast<float>(n) : 0.0f;
    };
    const float even = mean_radius(0.0f), crowded = mean_radius(1.0f);
    expect(crowded < even * 0.85f,
           "instance_brush: Centre Bias 1 pulls a clump's mean radius well inside the even "
           "distribution's -- one slider from a scattered copse to a tight clump");
}

COOPA_TEST(mixed_clusters_draw_from_the_first_n_prototypes_only) {
    InstanceBrushSettings s;
    s.prototype = 2;
    s.cluster_mixed = false;
    expect(cluster_prototype(s, 5, 123u) == 2,
           "instance_brush: without Mix Prototypes a clump is all the selected prototype");

    s.cluster_mixed = true;
    s.cluster_prototypes = 3;
    std::vector<int> seen;
    for (std::uint32_t seed = 1; seed < 200; ++seed) {
        const int p = cluster_prototype(s, 5, seed);
        expect(p >= 0 && p < 3,
               "instance_brush: a mixed clump only ever draws from the first N prototypes");
        if (std::find(seen.begin(), seen.end(), p) == seen.end()) seen.push_back(p);
    }
    expect(seen.size() == 3, "instance_brush: and over many clumps it uses all N");

    s.cluster_prototypes = 9;   // more than the palette has
    for (std::uint32_t seed = 1; seed < 50; ++seed) {
        expect(cluster_prototype(s, 2, seed) < 2,
               "instance_brush: Mix First N is clamped to the palette, so it never names a "
               "prototype that is not there");
    }
}

COOPA_TEST(a_record_takes_its_variation_from_the_prototypes_ranges) {
    const InstancePrototype p = pine();
    InstanceBrushSettings s;
    float lo = 1e9f, hi = -1e9f;
    for (std::uint32_t seed = 1; seed < 300; ++seed) {
        const auto r = make_record(p, 0, flat(glm::vec2(0.0f)), s, seed);
        expect(r.scale.x == r.scale.y && r.scale.y == r.scale.z,
               "instance_brush: an instance's scale is uniform");
        expect(r.scale.x >= 1.0f - 1e-4f && r.scale.x <= 2.0f + 1e-4f,
               "instance_brush: scale stays inside the prototype's range");
        lo = std::min(lo, r.scale.x);
        hi = std::max(hi, r.scale.x);
        expect_near(glm::length(r.rotation), 1.0f, 1e-4f,
                    "instance_brush: the rotation is a unit quaternion");
    }
    expect(lo < 1.15f && hi > 1.85f,
           "instance_brush: over many instances the scale range is actually used end to end, "
           "not clustered in its middle");
    expect(make_record(p, 0, flat(glm::vec2(0.0f)), s, 42u).seed == 42u,
           "instance_brush: the seed is stored, so a rebuild reproduces the same instance");
    expect(make_record(p, 3, flat(glm::vec2(0.0f)), s, 42u).prototype == 3,
           "instance_brush: the prototype index is stored as given");
}

COOPA_TEST(align_to_normal_interpolates_between_standing_up_and_lying_on_the_surface) {
    InstancePrototype p = pine();
    p.yaw_range = glm::vec2(0.0f);   // isolate the tilt
    InstanceBrushSettings s;
    SurfaceSample slope;
    slope.position = glm::vec3(0.0f);
    slope.normal = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));   // 45 degrees

    p.align_to_normal = 0.0f;
    glm::vec3 up = make_record(p, 0, slope, s, 5u).rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    expect_near(up.z, 1.0f, 1e-4f,
                "instance_brush: align_to_normal 0 stands an instance straight up however "
                "steep the ground is -- what a tree wants");

    p.align_to_normal = 1.0f;
    up = make_record(p, 0, slope, s, 5u).rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    expect_near(glm::length(up - slope.normal), 0.0f, 1e-4f,
                "instance_brush: align_to_normal 1 lies it flat on the surface -- what a rock wants");

    p.align_to_normal = 0.5f;
    up = make_record(p, 0, slope, s, 5u).rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    expect(up.z > slope.normal.z && up.z < 1.0f - 1e-4f,
           "instance_brush: half way is half way between the two");

    // A vertical wall is a valid surface: the rotation is built between two unit vectors rather
    // than from Euler angles, so nothing gimbal-locks at 90 degrees.
    SurfaceSample wall;
    wall.normal = glm::vec3(1.0f, 0.0f, 0.0f);
    p.align_to_normal = 1.0f;
    up = make_record(p, 0, wall, s, 5u).rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    expect_near(glm::length(up - wall.normal), 0.0f, 1e-4f,
                "instance_brush: a wall-facing normal is handled as cleanly as a gentle slope");
}

COOPA_TEST(sink_pushes_an_instance_into_the_surface_along_its_normal) {
    InstancePrototype p = pine();
    p.sink = 0.25f;
    p.scale_range = glm::vec2(2.0f, 2.0f);   // pin the scale so the arithmetic is exact
    InstanceBrushSettings s;
    const auto r = make_record(p, 0, flat(glm::vec2(10.0f, 20.0f)), s, 3u);
    expect_near(r.position.z, -0.5f, 1e-4f,
                "instance_brush: sink scales with the instance, so a big tree sinks "
                "proportionally (0.25 m x scale 2)");
    expect_near(r.position.x, 10.0f, 1e-4f, "instance_brush: sinking does not move it sideways");

    SurfaceSample wall;
    wall.position = glm::vec3(0.0f);
    wall.normal = glm::vec3(1.0f, 0.0f, 0.0f);
    const auto on_wall = make_record(p, 0, wall, s, 3u);
    expect_near(on_wall.position.x, -0.5f, 1e-4f,
                "instance_brush: sink runs along the surface normal, so it works on a wall too");
}

COOPA_TEST(snapped_yaw_is_quantised_to_the_step) {
    InstancePrototype p = pine();
    InstanceBrushSettings s;
    s.snap = true;
    s.yaw_step = 90.0f;
    for (std::uint32_t seed = 1; seed < 100; ++seed) {
        const auto r = make_record(p, 0, flat(glm::vec2(0.0f)), s, seed);
        // Recover the yaw and check it is a multiple of 90 degrees.
        const glm::vec3 fwd = r.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        const float yaw_deg = glm::degrees(std::atan2(fwd.y, fwd.x));
        const float snapped = std::round(yaw_deg / 90.0f) * 90.0f;
        expect_near(std::abs(yaw_deg - snapped), 0.0f, 0.05f,
                    "instance_brush: with Snap on, yaw lands on a multiple of Yaw Step -- which "
                    "is what makes a fence line up");
    }
}

COOPA_TEST(an_undo_patch_restores_the_chunks_a_stroke_touched) {
    using toy::editor::apply_instance_patch;
    using toy::editor::capture_instance_patch;
    using toy::instance::ChunkCoord;
    using toy::instance::InstanceRecord;
    using toy::instance::InstanceSetData;

    auto data = std::make_shared<InstanceSetData>();
    data->set_chunk_size(32.0f);
    const InstancePrototype p = pine();
    InstanceBrushSettings s;

    // A chunk that already holds something, so the patch has a non-empty "before" to restore.
    data->add(make_record(p, 0, flat(glm::vec2(5.0f, 5.0f)), s, 1u));
    const std::vector<ChunkCoord> touched{ChunkCoord(0, 0), ChunkCoord(1, 0)};
    const auto before = capture_instance_patch(data, touched);
    expect(before.chunks.size() == 2,
           "instance_brush: a patch covers every chunk it was asked about, including ones that "
           "held nothing -- a stroke that ADDS to an empty chunk must be able to empty it again");

    for (int i = 0; i < 20; ++i) {
        data->add(make_record(p, 0, flat(glm::vec2(6.0f + 0.1f * static_cast<float>(i), 5.0f)), s,
                              static_cast<std::uint32_t>(i + 10)));
    }
    data->add(make_record(p, 0, flat(glm::vec2(40.0f, 5.0f)), s, 99u));
    const auto after = capture_instance_patch(data, touched);
    expect(data->size() == 22, "instance_brush: the stroke added what it added");
    expect(before.bytes() < after.bytes(),
           "instance_brush: a patch's size is its records, which is what the undo budget meters");

    require(apply_instance_patch(before), "instance_brush: the before-patch applies");
    expect(data->size() == 1,
           "instance_brush: undoing the stroke leaves exactly the one instance that predated it");
    require(apply_instance_patch(after), "instance_brush: the after-patch applies");
    expect(data->size() == 22, "instance_brush: redoing it puts all of them back");

    // The painter can be deleted between the stroke and the undo; the patch holds its data
    // weakly so that is a skipped step, not a crash.
    const auto orphan = capture_instance_patch(data, touched);
    data.reset();
    expect(!apply_instance_patch(orphan),
           "instance_brush: a patch whose painter is gone reports failure instead of dereferencing it");
}
