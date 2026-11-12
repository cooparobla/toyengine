/**
 * @file curve_edit_test.cpp
 * @brief The pen tool's decisions: what is under the cursor, where a click lands in depth, and
 *        what a dragged handle does to its partner.
 *
 * All of it without a window, which is the point of keeping it in `editor/viewport/curve_edit`
 * rather than in the panel: the geometry is where a curve tool actually goes wrong.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include "editor/support/fixtures.h"
#include "editor/viewport/curve_edit.h"

COOPA_TEST_SUITE("curve_edit");

namespace toy::editor::testing {
namespace {

using namespace toy::editor::curve_tool;
namespace cv = toy::curve;

/// @brief A camera looking down -Z at the XY plane from 10 units up, over an 800x600 viewport.
ViewProj top_down() {
    ViewProj vp;
    vp.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    vp.proj = glm::perspective(glm::radians(50.0f), 800.0f / 600.0f, 0.1f, 100.0f);
    vp.proj[1][1] *= -1.0f;   // the engine's clip convention
    vp.rect = imm::Box{0.0f, 0.0f, 800.0f, 600.0f};
    return vp;
}

/// @brief Three points along +X in the XY plane, with straight (Vector) handles.
cv::CurveData flat_line() {
    cv::CurveData c;
    for (float x : {-2.0f, 0.0f, 2.0f}) {
        cv::CurvePoint p;
        p.position = glm::vec3(x, 0.0f, 0.0f);
        p.mode = cv::HandleMode::Vector;
        c.points.push_back(p);
    }
    cv::refresh_handles(c);
    return c;
}

} // namespace

/** @brief Clicking a control point finds that point, not the line running through it. */
COOPA_TEST(a_click_on_a_control_point_finds_the_point) {
    const ViewProj vp = top_down();
    const cv::CurveData c = flat_line();
    const glm::mat4 id(1.0f);

    const auto px = vp.project(c.points[1].position);
    expect(px.has_value(), "curve_edit: the middle point is on screen");

    const CurveHit hit = hit_test(c, vp, id, *px, 9.0f, /*show_handles=*/false);
    expect(hit.kind == HitKind::Point, "curve_edit: a point was hit, not the segment through it");
    expect(hit.index == 1, "curve_edit: and it is the one clicked");
}

/** @brief Clicking the drawn line finds the segment, and where along it. */
COOPA_TEST(a_click_on_the_line_finds_the_segment_and_where_along_it) {
    const ViewProj vp = top_down();
    const cv::CurveData c = flat_line();
    const glm::mat4 id(1.0f);

    // Midway along the second segment: x = 1, clear of both its control points.
    const auto px = vp.project(glm::vec3(1.0f, 0.0f, 0.0f));
    expect(px.has_value(), "curve_edit: the midpoint is on screen");

    const CurveHit hit = hit_test(c, vp, id, *px, 9.0f, false);
    expect(hit.kind == HitKind::Segment, "curve_edit: the line was hit");
    expect(hit.index == 1, "curve_edit: on the second segment");
    expect(hit.t > 0.2f && hit.t < 0.8f,
           "curve_edit: about halfway along it (t=" + std::to_string(hit.t) + ")");
}

/** @brief A handle outranks the line it sits on, or it could never be grabbed. */
COOPA_TEST(a_handle_outranks_the_line_it_lies_on) {
    const ViewProj vp = top_down();
    cv::CurveData c = flat_line();
    c.points[1].mode = cv::HandleMode::Free;
    c.points[1].out_handle = glm::vec3(0.8f, 0.0f, 0.0f);   // on the curve's own path
    const glm::mat4 id(1.0f);

    const auto px = vp.project(c.points[1].out_handle);
    const CurveHit on = hit_test(c, vp, id, *px, 9.0f, /*show_handles=*/true);
    expect(on.kind == HitKind::OutHandle && on.index == 1,
           "curve_edit: with handles shown, the handle wins over the line under it");

    const CurveHit off = hit_test(c, vp, id, *px, 9.0f, /*show_handles=*/false);
    expect(off.kind != HitKind::OutHandle,
           "curve_edit: with them hidden it is not pickable -- only what is drawn can be hit");
}

/** @brief Empty space hits nothing. */
COOPA_TEST(a_click_on_nothing_hits_nothing) {
    const ViewProj vp = top_down();
    const cv::CurveData c = flat_line();
    const CurveHit hit = hit_test(c, vp, glm::mat4(1.0f), glm::vec2(10.0f, 580.0f), 9.0f, true);
    expect(!hit, "curve_edit: a click in the corner finds nothing");
    expect(hit.kind == HitKind::None, "curve_edit: and says so");
}

/** @brief The object's transform is honoured: a moved curve is hit where it is drawn. */
COOPA_TEST(hit_testing_follows_the_objects_transform) {
    const ViewProj vp = top_down();
    const cv::CurveData c = flat_line();
    const glm::mat4 to_world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, 0.0f));

    const auto moved_px = vp.project(glm::vec3(0.0f, 3.0f, 0.0f));
    expect(hit_test(c, vp, to_world, *moved_px, 9.0f, false).kind == HitKind::Point,
           "curve_edit: the curve is hit where the object put it");

    const auto origin_px = vp.project(glm::vec3(0.0f, 0.0f, 0.0f));
    expect(hit_test(c, vp, to_world, *origin_px, 9.0f, false).kind != HitKind::Point,
           "curve_edit: and not where its local coordinates alone would say");
}

/** @brief A click resolves to the ground plane, which is how a path is laid over terrain. */
COOPA_TEST(a_click_resolves_onto_the_ground_plane) {
    const ViewProj vp = top_down();
    const auto px = vp.project(glm::vec3(1.5f, -0.5f, 0.0f));
    expect(px.has_value(), "curve_edit: the probe point is on screen");

    const auto got = point_on_ground(vp, *px, 0.0f);
    expect(got.has_value(), "curve_edit: the ray meets the plane");
    expect(glm::distance(*got, glm::vec3(1.5f, -0.5f, 0.0f)) < 1e-2f,
           "curve_edit: at the point that projected there, so a click lands where it looked");
    coopa::test::expect_near(got->z, 0.0f, 1e-5f, "curve_edit: exactly on the plane");
}

/** @brief A camera-facing plane keeps the depth of what it was anchored to. */
COOPA_TEST(the_camera_plane_keeps_the_anchors_depth) {
    const ViewProj vp = top_down();
    const glm::vec3 anchor(0.0f, 0.0f, 2.0f);
    const auto px = vp.project(glm::vec3(1.0f, 1.0f, 2.0f));
    const glm::vec3 got = point_on_camera_plane(vp, *px, anchor);
    coopa::test::expect_near(got.z, 2.0f, 1e-3f,
                "curve_edit: the new point shares the anchor's depth rather than snapping to 0");
}

/** @brief Dragging an Aligned handle mirrors its direction but keeps the other's length. */
COOPA_TEST(an_aligned_handle_mirrors_direction_but_not_length) {
    cv::CurveData c = flat_line();
    c.points[1].mode = cv::HandleMode::Aligned;
    c.points[1].in_handle  = glm::vec3(-0.5f, 0.0f, 0.0f);   // short
    c.points[1].out_handle = glm::vec3(2.0f, 0.0f, 0.0f);    // long
    const float in_len_before = glm::length(c.points[1].in_handle - c.points[1].position);

    drag_handle(c, 1, /*out_handle=*/true, glm::vec3(0.0f, 1.0f, 0.0f));

    const glm::vec3 out_arm = c.points[1].out_handle - c.points[1].position;
    const glm::vec3 in_arm  = c.points[1].in_handle - c.points[1].position;
    expect(glm::dot(glm::normalize(out_arm), glm::normalize(in_arm)) < -0.999f,
           "curve_edit: the two handles point opposite ways");
    coopa::test::expect_near(glm::length(in_arm), in_len_before, 1e-4f,
                "curve_edit: and the untouched handle kept ITS length -- mirroring that too "
                "would snap a short handle long the moment the other was grabbed");
}

/** @brief Dragging a derived handle makes the point Free; it cannot be both. */
COOPA_TEST(dragging_a_derived_handle_makes_the_point_free) {
    cv::CurveData c = flat_line();   // every point is Vector
    expect(c.points[1].mode == cv::HandleMode::Vector, "curve_edit: it starts derived");

    drag_handle(c, 1, true, glm::vec3(0.0f, 1.5f, 0.0f));
    expect(c.points[1].mode == cv::HandleMode::Free,
           "curve_edit: a handle moved by hand cannot also be computed from the neighbours");
    expect(glm::distance(c.points[1].out_handle, glm::vec3(0.0f, 1.5f, 0.0f)) < 1e-5f,
           "curve_edit: and it went where it was dragged");

    cv::refresh_handles(c);
    expect(glm::distance(c.points[1].out_handle, glm::vec3(0.0f, 1.5f, 0.0f)) < 1e-5f,
           "curve_edit: a later refresh leaves it alone, which is what Free means");
}

/** @brief Endpoints are known, so a click can extend an open curve rather than insert. */
COOPA_TEST(endpoints_are_recognised_on_an_open_curve_only) {
    cv::CurveData c = flat_line();
    expect(is_endpoint(c, 0) && is_endpoint(c, 2), "curve_edit: both ends of an open curve");
    expect(!is_endpoint(c, 1), "curve_edit: but not the middle");
    c.closed = true;
    expect(!is_endpoint(c, 0) && !is_endpoint(c, 2),
           "curve_edit: a closed curve has no ends to extend from");
}


/** @brief A rubber band picks the anchors inside it, from either corner. */
COOPA_TEST(a_band_picks_the_anchors_inside_it_dragged_either_way) {
    const ViewProj vp = top_down();
    const cv::CurveData c = flat_line();   // anchors at x = -2, 0, +2 on the XY plane
    const glm::mat4 id(1.0f);

    const auto left = vp.project(glm::vec3(-2.0f, 0.0f, 0.0f));
    const auto mid = vp.project(glm::vec3(0.0f, 0.0f, 0.0f));
    const auto right = vp.project(glm::vec3(2.0f, 0.0f, 0.0f));
    expect(left && mid && right, "curve_edit: all three anchors are on screen");

    // A box around the left two only, with a margin that cannot reach the third.
    const float cut = (mid->x + right->x) * 0.5f;
    const auto picked = anchors_in_box(c, vp, id, {left->x - 20.0f, left->y - 40.0f},
                                       {cut, left->y + 40.0f});
    expect(picked.size() == 2, "curve_edit: two anchors in the band (" +
                                   std::to_string(picked.size()) + ")");
    expect(picked[0] == 0 && picked[1] == 1, "curve_edit: and they are the left two");

    // The same rectangle dragged from the opposite corner must pick the same anchors.
    const auto flipped = anchors_in_box(c, vp, id, {cut, left->y + 40.0f},
                                        {left->x - 20.0f, left->y - 40.0f});
    expect(flipped == picked, "curve_edit: corners in either order give the same result");

    expect(anchors_in_box(c, vp, id, {5.0f, 5.0f}, {25.0f, 25.0f}).empty(),
           "curve_edit: a band over nothing picks nothing");
}

}  // namespace toy::editor::testing
