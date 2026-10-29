/**
 * @file curve_data_test.cpp
 * @brief Cubic Bezier evaluation, arc-length spacing, and the handle rules the pen tool relies on.
 *
 * Every decision about where a curve point goes lives in `curve_data`, which is why it is a pure
 * header-and-cpp with no scene: the viewport draws `polyline()` and a bake instances on
 * `points_along()`, so the two agree by construction rather than by two implementations matching.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include "engine/support/checks.h"

#include <toyengine/curve/curve_data.h>

COOPA_TEST_SUITE("curve_data");

namespace {

using namespace toy::curve;

/// @brief A straight run along +X with Vector handles, so the cubic draws a line.
CurveData straight(int points = 3, float spacing = 1.0f) {
    CurveData c;
    for (int i = 0; i < points; ++i) {
        CurvePoint p;
        p.position = glm::vec3(static_cast<float>(i) * spacing, 0.0f, 0.0f);
        p.mode = HandleMode::Vector;
        c.points.push_back(p);
    }
    refresh_handles(c);
    return c;
}

} // namespace

/** @brief A curve needs two points before it is a curve at all. */
COOPA_TEST(a_curve_with_fewer_than_two_points_has_no_segments) {
    CurveData c;
    expect(c.empty() && c.segment_count() == 0, "curve: nothing is not a curve");
    append_point(c, glm::vec3(0.0f));
    expect(c.empty() && c.segment_count() == 0, "curve: one point is still not a curve");
    append_point(c, glm::vec3(1.0f, 0.0f, 0.0f));
    expect(!c.empty() && c.segment_count() == 1, "curve: two points make one segment");
    c.closed = true;
    expect(c.segment_count() == 2, "curve: closing adds the segment back to the start");
}

/** @brief Vector handles on both ends make the segment a straight line. */
COOPA_TEST(vector_handles_draw_a_straight_line) {
    const CurveData c = straight(2, 2.0f);
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const glm::vec3 p = evaluate_segment(c, 0, t);
        expect_near(p.x, t * 2.0f, 1e-4f, "curve: x advances evenly at t=" + std::to_string(t));
        expect_near(p.y, 0.0f, 1e-5f, "curve: and never leaves the axis");
    }
    expect_near(length(c), 2.0f, 1e-3f, "curve: its length is the distance between the points");
}

/** @brief Inserting a point refines the curve without moving it. */
COOPA_TEST(inserting_a_point_leaves_the_shape_exactly_where_it_was) {
    CurveData c;
    append_point(c, glm::vec3(0.0f, 0.0f, 0.0f));
    append_point(c, glm::vec3(2.0f, 0.0f, 0.0f));
    c.points[0].mode = HandleMode::Free;
    c.points[1].mode = HandleMode::Free;
    c.points[0].out_handle = glm::vec3(0.0f, 2.0f, 0.0f);   // a strong arc, so a nudge would show
    c.points[1].in_handle  = glm::vec3(2.0f, 2.0f, 0.0f);

    std::vector<glm::vec3> before;
    for (int i = 0; i <= 20; ++i) before.push_back(evaluate_segment(c, 0, i / 20.0f));

    const std::size_t at = insert_point(c, 0, 0.5f);
    expect(at == 1 && c.points.size() == 3, "curve: the point landed between the two");
    expect(c.segment_count() == 2, "curve: and split the segment in two");

    // De Casteljau's promise: the two halves reproduce the original curve exactly.
    for (int i = 0; i <= 20; ++i) {
        const float t = i / 20.0f;
        const glm::vec3 now = t <= 0.5f ? evaluate_segment(c, 0, t * 2.0f)
                                        : evaluate_segment(c, 1, (t - 0.5f) * 2.0f);
        expect(glm::distance(now, before[static_cast<std::size_t>(i)]) < 1e-4f,
               "curve: the shape is unchanged at t=" + std::to_string(t) +
                   " (moved " + std::to_string(glm::distance(now, before[static_cast<std::size_t>(i)])) + ")");
    }
}

/** @brief Points are spread by arc length, not by parameter. */
COOPA_TEST(points_along_spaces_by_distance_not_by_parameter) {
    // Segments of 1 and 4 units: even `t` would put the same count on each, bunching the short one.
    CurveData c;
    for (float x : {0.0f, 1.0f, 5.0f}) {
        CurvePoint p;
        p.position = glm::vec3(x, 0.0f, 0.0f);
        p.mode = HandleMode::Vector;
        c.points.push_back(p);
    }
    refresh_handles(c);

    std::vector<glm::vec3> tangents;
    const std::vector<glm::vec3> pts = points_along(c, 6, &tangents);
    expect(pts.size() == 6, "curve: six points came back");
    expect(tangents.size() == 6, "curve: with a tangent each");

    for (std::size_t i = 1; i < pts.size(); ++i) {
        expect_near(glm::distance(pts[i - 1], pts[i]), 1.0f, 0.02f,
                    "curve: gap " + std::to_string(i) + " is one unit, so the long segment got "
                    "the points the short one did not");
    }
    expect_near(pts.front().x, 0.0f, 1e-4f, "curve: the first point is the start");
    expect_near(pts.back().x, 5.0f, 1e-3f, "curve: the last is the end");
    expect(tangents[0].x > 0.99f, "curve: running along +X");
}

/** @brief Fewer than two points along is not a spacing question; it yields nothing. */
COOPA_TEST(points_along_refuses_degenerate_counts) {
    const CurveData c = straight();
    expect(points_along(c, 1).empty(), "curve: one point has no spacing to speak of");
    expect(points_along(c, 0).empty(), "curve: nor zero");
    expect(points_along(CurveData{}, 8).empty(), "curve: and an empty curve yields nothing");
}

/** @brief A closed curve's samples wrap, and its last point does not sit on its first. */
COOPA_TEST(a_closed_curve_wraps_without_doubling_its_seam) {
    CurveData c;
    for (auto p : {glm::vec3(0, 0, 0), glm::vec3(4, 0, 0), glm::vec3(4, 4, 0), glm::vec3(0, 4, 0)}) {
        CurvePoint cp;
        cp.position = p;
        cp.mode = HandleMode::Vector;
        c.points.push_back(cp);
    }
    c.closed = true;
    refresh_handles(c);

    expect(c.segment_count() == 4, "curve: four sides");
    expect_near(length(c), 16.0f, 0.05f, "curve: the perimeter of a 4x4 square");

    const std::vector<glm::vec3> pts = points_along(c, 8);
    expect(pts.size() == 8, "curve: eight points around it");
    expect(glm::distance(pts.front(), pts.back()) > 1.0f,
           "curve: the last point does not land on the first -- a closed loop spreads over the "
           "whole perimeter rather than stopping short of it");
}

/**
 * @brief Moving a point carries STORED handles, and reshapes the derived ones beside it.
 *
 * The two halves of the rule, and they differ by mode: a Free/Aligned point owns its handles, so
 * they travel with it, while a Vector/Auto point derives them from its neighbours and therefore
 * recomputes rather than translates. Asserting only the first would make `refresh_handles()`
 * look broken; asserting only the second would let a dragged corner lose its shape.
 */
COOPA_TEST(moving_a_point_carries_stored_handles_and_reshapes_derived_neighbours) {
    CurveData c = straight(3);
    c.points[0].mode = HandleMode::Auto;      // derived: must reshape when its neighbour moves
    c.points[1].mode = HandleMode::Aligned;   // stored: must travel with the point
    c.points[2].mode = HandleMode::Auto;
    refresh_handles(c);
    const glm::vec3 neighbour_before = c.points[0].out_handle;

    const glm::vec3 offset(0.0f, 3.0f, 0.0f);
    const glm::vec3 handle_before = c.points[1].out_handle;
    move_point(c, 1, offset);

    expect_near(c.points[1].position.y, 3.0f, 1e-5f, "curve: the point moved");
    expect(glm::distance(c.points[1].out_handle, handle_before + offset) < 1e-4f,
           "curve: its stored handle came with it rather than staying behind");
    expect(glm::distance(c.points[0].out_handle, neighbour_before) > 1e-3f,
           "curve: and the Auto neighbour reshaped, because its handles are derived from where "
           "this point now is");
}

/** @brief A Vector point's handles are derived, so a move recomputes rather than translates. */
COOPA_TEST(a_derived_points_handles_are_recomputed_not_dragged) {
    CurveData c = straight(3);   // every point is Vector
    const glm::vec3 handle_before = c.points[1].out_handle;
    move_point(c, 1, glm::vec3(0.0f, 3.0f, 0.0f));

    expect_near(c.points[1].position.y, 3.0f, 1e-5f, "curve: the point moved");
    expect(glm::distance(c.points[1].out_handle, handle_before + glm::vec3(0.0f, 3.0f, 0.0f)) > 1e-3f,
           "curve: a Vector handle did NOT simply translate -- it points at the neighbour, which "
           "did not move");
    const glm::vec3 toward_next = glm::normalize(c.points[2].position - c.points[1].position);
    const glm::vec3 handle_dir = glm::normalize(c.points[1].out_handle - c.points[1].position);
    expect(glm::dot(toward_next, handle_dir) > 0.999f,
           "curve: it still points straight at the next control point");
}

/** @brief A tangent frame is orthonormal, and survives a curve running straight up. */
COOPA_TEST(the_tangent_frame_stays_sane_even_pointing_at_the_up_axis) {
    for (const glm::vec3& dir : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1),
                                 glm::vec3(0, 0, -1), glm::vec3(1, 1, 1)}) {
        const glm::quat q = frame_from_tangent(glm::normalize(dir), 0.0f);
        const glm::mat3 m = glm::mat3_cast(q);
        expect_near(glm::length(m[0]), 1.0f, 1e-4f, "curve: the frame is normalised");
        expect(std::abs(glm::dot(m[0], m[1])) < 1e-3f, "curve: and orthogonal");
        expect(glm::distance(m[0], glm::normalize(dir)) < 1e-3f,
               "curve: with +X along the tangent");
    }
}
