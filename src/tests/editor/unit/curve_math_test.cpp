/**
 * @file curve_math_test.cpp
 * @brief The curve and gradient editors' maths: sampling, picking, inserting, dragging,
 * removing, and the YAML both round-trip through.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <vector>

#include "editor/support/fixtures.h"
#include "editor/ui/curve_math.h"

COOPA_TEST_SUITE("curve_math");

namespace toy::editor::testing {

using namespace toy::editor::ui;

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

const CurveBox kBox{10.0f, 20.0f, 100.0f, 50.0f};   // 100 x 50 at (10, 20)

} // namespace

/** @brief The unit square maps into the box with Y flipped, and back again. */
COOPA_TEST(curve_space_maps_both_ways_with_y_up) {
    expect(curve_to_box(kBox, {0.0f, 0.0f}) == glm::vec2(10.0f, 70.0f), "curve (0,0) is the box's bottom-left");
    expect(curve_to_box(kBox, {1.0f, 1.0f}) == glm::vec2(110.0f, 20.0f), "curve (1,1) is its top-right");
    expect(curve_to_box(kBox, {0.5f, 0.5f}) == glm::vec2(60.0f, 45.0f), "the middle is the middle");

    const glm::vec2 back = box_to_curve(kBox, {60.0f, 45.0f});
    expect(near(back.x, 0.5f) && near(back.y, 0.5f), "and the inverse returns it");
    const glm::vec2 outside = box_to_curve(kBox, {-500.0f, 5000.0f});
    expect(outside.x == 0.0f && outside.y == 0.0f, "a point outside the box clamps into the unit square");
}

/** @brief A curve is piecewise linear, and holds its end values rather than extrapolating. */
COOPA_TEST(sampling_interpolates_between_points_and_holds_the_ends) {
    const std::vector<glm::vec2> pts = {{0.0f, 0.2f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    expect(near(sample_curve(pts, 0.0f), 0.2f), "at the first point");
    expect(near(sample_curve(pts, 0.25f), 0.6f), "halfway up the first leg");
    expect(near(sample_curve(pts, 0.5f), 1.0f), "at the peak");
    expect(near(sample_curve(pts, 0.75f), 0.5f), "halfway down the second");
    expect(near(sample_curve(pts, -3.0f), 0.2f), "before the curve it holds the first value");
    expect(near(sample_curve(pts, 9.0f), 0.0f), "after it, the last");
    expect(sample_curve({}, 0.5f) == 0.0f, "an empty curve is flat zero");
}

/** @brief A gradient interpolates its stops, channel by channel. */
COOPA_TEST(a_gradient_interpolates_between_its_stops) {
    const std::vector<ColorStop> stops = {{0.0f, {1.0f, 0.0f, 0.0f}}, {1.0f, {0.0f, 0.0f, 1.0f}}};
    const glm::vec3 mid = sample_gradient(stops, 0.5f);
    expect(near(mid.r, 0.5f) && near(mid.g, 0.0f) && near(mid.b, 0.5f), "halfway is half of each end");
    expect(sample_gradient(stops, -1.0f) == glm::vec3(1, 0, 0), "before the first stop is the first colour");
    expect(sample_gradient(stops, 2.0f) == glm::vec3(0, 0, 1), "after the last is the last");
    expect(sample_gradient({}, 0.5f) == glm::vec3(0.0f), "a gradient with no stops is black");
}

/** @brief Picking takes the nearest point within the handle radius, and nothing beyond it. */
COOPA_TEST(picking_finds_the_nearest_handle_within_reach) {
    const std::vector<glm::vec2> pts = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    expect(pick_curve_point(pts, kBox, {60.0f, 20.0f}) == 1, "right on the peak handle");
    expect(pick_curve_point(pts, kBox, {63.0f, 23.0f}) == 1, "a few pixels off still hits it");
    expect(pick_curve_point(pts, kBox, {60.0f, 45.0f}) == -1, "the middle of the box hits nothing");
    expect(pick_curve_point({}, kBox, {60.0f, 20.0f}) == -1, "an empty curve has nothing to pick");

    const std::vector<ColorStop> stops = {{0.0f, {}}, {0.5f, {}}, {1.0f, {}}};
    expect(pick_gradient_stop(stops, kBox, {60.0f, 40.0f}) == 1, "a stop is picked by x alone");
    expect(pick_gradient_stop(stops, kBox, {85.0f, 40.0f}) == -1, "...and only within reach");
}

/** @brief Inserting keeps the list sorted by x. */
COOPA_TEST(inserting_a_point_keeps_the_curve_sorted) {
    std::vector<glm::vec2> pts = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    expect(insert_curve_point(pts, {0.4f, 0.8f}) == 1, "it lands between the two");
    expect(pts.size() == 3 && near(pts[1].x, 0.4f), "...and is where it was put");
    insert_curve_point(pts, {0.2f, 0.1f});
    expect(near(pts[1].x, 0.2f) && near(pts[2].x, 0.4f), "a later insert still sorts");
    insert_curve_point(pts, {5.0f, -2.0f});
    expect(near(pts.back().x, 1.0f) && near(pts.back().y, 0.0f), "a point outside the square clamps into it");
}

/**
 * @brief Dragging clamps into the square and between the neighbours, and the end points keep
 *        their x so the curve always spans the whole range.
 */
COOPA_TEST(dragging_a_point_cannot_cross_its_neighbours) {
    std::vector<glm::vec2> pts = {{0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f}};
    expect(move_curve_point(pts, 1, {0.7f, 0.2f}), "the middle point moves");
    expect(near(pts[1].x, 0.7f) && near(pts[1].y, 0.2f), "...to where it was dragged");
    move_curve_point(pts, 1, {5.0f, 5.0f});
    expect(pts[1].x < 1.0f && near(pts[1].y, 1.0f), "dragged past the last point it stops just short of it");
    move_curve_point(pts, 1, {-5.0f, -5.0f});
    expect(pts[1].x > 0.0f && near(pts[1].y, 0.0f), "and past the first, just after it");

    move_curve_point(pts, 0, {0.4f, 1.0f});
    expect(near(pts[0].x, 0.0f) && near(pts[0].y, 1.0f), "the first point moves in y only");
    move_curve_point(pts, 2, {0.4f, 0.0f});
    expect(near(pts[2].x, 1.0f), "and so does the last");
    expect(!move_curve_point(pts, 7, {0.5f, 0.5f}), "an index off the end moves nothing");
}

/** @brief A curve keeps at least two points; a gradient at least two stops. */
COOPA_TEST(removing_never_empties_a_curve_or_a_gradient) {
    std::vector<glm::vec2> pts = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    expect(remove_curve_point(pts, 1) && pts.size() == 2, "the middle point goes");
    expect(!remove_curve_point(pts, 0), "the last two stay");
    expect(!remove_curve_point(pts, 9), "an index off the end removes nothing");

    std::vector<ColorStop> stops = {{0.0f, {}}, {0.5f, {}}, {1.0f, {}}};
    expect(remove_gradient_stop(stops, 1) && stops.size() == 2, "a stop goes");
    expect(!remove_gradient_stop(stops, 1), "...until only two are left");
}

/** @brief A stop inserted mid-gradient takes the colour already there, and dragging re-sorts. */
COOPA_TEST(gradient_stops_insert_in_place_and_re_sort_when_dragged) {
    std::vector<ColorStop> stops = {{0.0f, {1.0f, 0.0f, 0.0f}}, {1.0f, {0.0f, 0.0f, 1.0f}}};
    const int at = insert_gradient_stop(stops, 0.5f);
    expect(at == 1 && stops.size() == 3, "it lands between the ends");
    expect(near(stops[1].color.r, 0.5f) && near(stops[1].color.b, 0.5f), "with the colour the gradient already had there");

    const int moved = move_gradient_stop(stops, 1, 1.0f);
    expect(moved == 2, "dragged past the end it becomes the last stop");
    expect(near(stops[2].color.r, 0.5f), "...carrying its colour with it");
}

/** @brief Both shapes round-trip through the YAML the engine reads. */
COOPA_TEST(curves_and_gradients_round_trip_through_yaml) {
    const std::vector<glm::vec2> pts = {{0.0f, 0.25f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    const std::vector<glm::vec2> back = curve_from_node(curve_to_node(pts));
    expect(back.size() == 3 && near(back[1].x, 0.5f) && near(back[1].y, 1.0f), "a curve survives the trip");

    Node unsorted = Node::sequence();
    const float b[2] = {0.8f, 0.1f};
    const float a[2] = {0.2f, 0.9f};
    unsorted.as_seq().push_back(make_float_seq(b, 2));
    unsorted.as_seq().push_back(make_float_seq(a, 2));
    const std::vector<glm::vec2> sorted = curve_from_node(unsorted);
    expect(sorted.size() == 2 && near(sorted[0].x, 0.2f), "a hand-written file out of order is sorted on load");
    expect(curve_from_node(Node(std::string("not a curve"))).empty(), "a malformed node reads as an empty curve");

    const std::vector<ColorStop> stops = {{0.0f, {1, 0, 0}}, {0.75f, {0, 1, 0}}};
    const std::vector<ColorStop> gback = gradient_from_node(gradient_to_node(stops));
    expect(gback.size() == 2 && near(gback[1].t, 0.75f) && near(gback[1].color.g, 1.0f), "a gradient survives too");
}

} // namespace toy::editor::testing
