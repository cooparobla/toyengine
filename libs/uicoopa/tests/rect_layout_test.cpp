/**
 * @file rect_layout_test.cpp
 * @brief RectTransform layout math: resolve_rect() anchors/pivots/stretch, offset round-trips,
 *        Rect containment/intersection, LayoutElement measure semantics, world corners, and
 *        clamp_inside(). Canvas space is +Y up throughout.
 *
 * Not covered here: layout groups (layout_group_test.cpp) and the canvas-driven measure/arrange
 * pass (canvas_test.cpp).
 */

#include <coopa/testing/test.h>

#include "support/ui_test_support.h"

COOPA_TEST_SUITE("rect_layout");

static const Rect kParent{ glm::vec2(0.0f, 0.0f), glm::vec2(1000.0f, 500.0f) };

COOPA_TEST(anchor_presets_place_rect_at_corners_and_centre) {
    // Bottom-left, non-stretching, pivot at own corner: size_delta IS the absolute size.
    {
        RectParams p;
        p.anchor_min = p.anchor_max = { 0.0f, 0.0f };
        p.pivot = { 0.0f, 0.0f };
        p.anchored_position = { 10.0f, 20.0f };
        p.size_delta = { 100.0f, 50.0f };
        Rect r = resolve_rect(kParent, p);
        ASSERT_VEC_NEAR(r.min, glm::vec2(10.0f, 20.0f), 1e-4f);
        ASSERT_VEC_NEAR(r.max, glm::vec2(110.0f, 70.0f), 1e-4f);
    }
    // Top-right corner.
    {
        RectParams p;
        p.anchor_min = p.anchor_max = { 1.0f, 1.0f };
        p.pivot = { 1.0f, 1.0f };
        p.anchored_position = { -10.0f, -20.0f };
        p.size_delta = { 100.0f, 50.0f };
        Rect r = resolve_rect(kParent, p);
        ASSERT_VEC_NEAR(r.max, glm::vec2(990.0f, 480.0f), 1e-4f);
        ASSERT_VEC_NEAR(r.min, glm::vec2(890.0f, 430.0f), 1e-4f);
    }
    // Dead center.
    {
        RectParams p;
        p.anchor_min = p.anchor_max = { 0.5f, 0.5f };
        p.pivot = { 0.5f, 0.5f };
        p.anchored_position = { 0.0f, 0.0f };
        p.size_delta = { 200.0f, 100.0f };
        Rect r = resolve_rect(kParent, p);
        ASSERT_VEC_NEAR(r.center(), kParent.center(), 1e-4f);
        ASSERT_VEC_NEAR(r.size(), glm::vec2(200.0f, 100.0f), 1e-4f);
    }
}

COOPA_TEST(stretched_anchors_fill_parent_minus_size_delta) {
    // Stretch to fill parent exactly with zero size_delta and zero anchored_position.
    {
        RectParams p;
        p.anchor_min = { 0.0f, 0.0f };
        p.anchor_max = { 1.0f, 1.0f };
        p.pivot = { 0.5f, 0.5f };
        p.anchored_position = { 0.0f, 0.0f };
        p.size_delta = { 0.0f, 0.0f };
        Rect r = resolve_rect(kParent, p);
        ASSERT_VEC_NEAR(r.min, kParent.min, 1e-4f);
        ASSERT_VEC_NEAR(r.max, kParent.max, 1e-4f);
    }
    // Stretch horizontally with symmetric margins via negative size_delta.
    {
        RectParams p;
        p.anchor_min = { 0.0f, 0.5f };
        p.anchor_max = { 1.0f, 0.5f };
        p.pivot = { 0.5f, 0.5f };
        p.anchored_position = { 0.0f, 0.0f };
        p.size_delta = { -40.0f, 60.0f };  // 20px margin each side, 60px tall
        Rect r = resolve_rect(kParent, p);
        ASSERT_NEAR(r.min.x, kParent.min.x + 20.0f, 1e-4f);
        ASSERT_NEAR(r.max.x, kParent.max.x - 20.0f, 1e-4f);
        ASSERT_NEAR(r.size().y, 60.0f, 1e-4f);
    }
    // Stretch bottom bar: full width, fixed height pinned to the bottom edge.
    {
        RectParams p;
        p.anchor_min = { 0.0f, 0.0f };
        p.anchor_max = { 1.0f, 0.0f };
        p.pivot = { 0.5f, 0.0f };
        p.anchored_position = { 0.0f, 0.0f };
        p.size_delta = { 0.0f, 80.0f };
        Rect r = resolve_rect(kParent, p);
        ASSERT_VEC_NEAR(r.min, kParent.min, 1e-4f);
        ASSERT_NEAR(r.max.x, kParent.max.x, 1e-4f);
        ASSERT_NEAR(r.size().y, 80.0f, 1e-4f);
    }
}

COOPA_TEST(set_offsets_round_trips_offset_min_and_max) {
    RectParams p;
    p.anchor_min = { 0.0f, 0.0f };
    p.anchor_max = { 1.0f, 1.0f };
    p.pivot = { 0.5f, 0.5f };
    p.anchored_position = { 3.0f, -7.0f };
    p.size_delta = { -20.0f, -30.0f };

    glm::vec2 want_min = offset_min(kParent, p);
    glm::vec2 want_max = offset_max(kParent, p);

    RectParams p2;
    p2.anchor_min = p.anchor_min;
    p2.anchor_max = p.anchor_max;
    p2.pivot = p.pivot;
    // Deliberately garbage starting anchored_position/size_delta.
    p2.anchored_position = { 999.0f, -999.0f };
    p2.size_delta = { 12345.0f, -6789.0f };

    set_offsets(kParent, p2, want_min, want_max);

    ASSERT_VEC_NEAR(p2.anchored_position, p.anchored_position, 1e-3f);
    ASSERT_VEC_NEAR(p2.size_delta, p.size_delta, 1e-3f);
    ASSERT_VEC_NEAR(offset_min(kParent, p2), want_min, 1e-3f);
    ASSERT_VEC_NEAR(offset_max(kParent, p2), want_max, 1e-3f);
}

COOPA_TEST(anchored_position_places_the_pivot_point) {
    // Same anchored_position, different pivots -> different resolved rects,
    // but the pivot-referenced point itself must land in the same place.
    glm::vec2 anchored_pos{ 50.0f, 25.0f };
    glm::vec2 size{ 80.0f, 40.0f };

    glm::vec2 pivots[3] = { {0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f} };
    for (const glm::vec2& pivot : pivots) {
        RectParams p;
        p.anchor_min = p.anchor_max = { 0.5f, 0.5f };
        p.pivot = pivot;
        p.anchored_position = anchored_pos;
        p.size_delta = size;
        Rect r = resolve_rect(kParent, p);

        glm::vec2 anchor_point = kParent.center();
        glm::vec2 expected_pivot_world = anchor_point + anchored_pos;
        glm::vec2 actual_pivot_world = r.min + r.size() * pivot;
        ASSERT_VEC_NEAR(actual_pivot_world, expected_pivot_world, 1e-3f);
        ASSERT_VEC_NEAR(r.size(), size, 1e-4f);
    }
}

COOPA_TEST(layout_element_negative_override_means_unset) {
    LayoutElement le;
    le.min_size = { 10.0f, -1.0f };
    le.preferred_size = { 50.0f, 20.0f };
    le.flexible_size = { -1.0f, 1.0f };

    SizeConstraints c = le.measure();
    ASSERT_NEAR(c.min.x, 10.0f, 1e-4f);
    ASSERT_NEAR(c.min.y, 0.0f, 1e-4f);   // -1 override collapses to 0 via glm::max, i.e. "unset"
    ASSERT_VEC_NEAR(c.preferred, glm::vec2(50.0f, 20.0f), 1e-4f);
    ASSERT_NEAR(c.flexible.x, 0.0f, 1e-4f);
    ASSERT_NEAR(c.flexible.y, 1.0f, 1e-4f);
}

COOPA_TEST(contains_is_inclusive_and_disjoint_intersect_is_not_inverted) {
    Rect r{ glm::vec2(0.0f, 0.0f), glm::vec2(100.0f, 100.0f) };
    ASSERT_TRUE(contains(r, glm::vec2(50.0f, 50.0f)));
    ASSERT_TRUE(contains(r, glm::vec2(0.0f, 0.0f)));
    ASSERT_TRUE(contains(r, glm::vec2(100.0f, 100.0f)));
    ASSERT_TRUE(!contains(r, glm::vec2(-1.0f, 50.0f)));
    ASSERT_TRUE(!contains(r, glm::vec2(50.0f, 101.0f)));

    Rect a{ glm::vec2(0.0f, 0.0f), glm::vec2(60.0f, 60.0f) };
    Rect b{ glm::vec2(40.0f, 40.0f), glm::vec2(100.0f, 100.0f) };
    Rect i = intersect(a, b);
    ASSERT_VEC_NEAR(i.min, glm::vec2(40.0f, 40.0f), 1e-4f);
    ASSERT_VEC_NEAR(i.max, glm::vec2(60.0f, 60.0f), 1e-4f);

    // Non-overlapping rects collapse to a degenerate (zero-area) rect, not an inverted one.
    Rect c{ glm::vec2(0.0f, 0.0f), glm::vec2(10.0f, 10.0f) };
    Rect d{ glm::vec2(20.0f, 20.0f), glm::vec2(30.0f, 30.0f) };
    Rect none = intersect(c, d);
    ASSERT_TRUE(none.max.x >= none.min.x);
    ASSERT_TRUE(none.max.y >= none.min.y);
}

COOPA_TEST(world_corners_match_rect_and_rotate_about_pivot) {
    SceneObject obj("Widget");
    auto* rt = obj.add_component<RectTransform>();
    rt->set_anchor_min({0.0f, 0.0f});
    rt->set_anchor_max({0.0f, 0.0f});
    rt->set_pivot({0.0f, 0.0f});
    rt->set_anchored_position({10.0f, 20.0f});
    rt->set_size_delta({30.0f, 40.0f});
    rt->resolve(Rect{ glm::vec2(0.0f), glm::vec2(500.0f, 500.0f) });

    auto corners = rt->world_corners();
    // Unrotated/unscaled: world_matrix() is identity, so corners equal rect() directly.
    ASSERT_VEC_NEAR(corners[0], glm::vec2(10.0f, 20.0f), 1e-3f);   // bottom-left
    ASSERT_VEC_NEAR(corners[2], glm::vec2(40.0f, 60.0f), 1e-3f);   // top-right

    rt->set_local_rotation_degrees(90.0f);
    rt->resolve(Rect{ glm::vec2(0.0f), glm::vec2(500.0f, 500.0f) });
    auto rotated = rt->world_corners();
    // A 90-degree rotation about the pivot (bottom-left corner here) must preserve that corner.
    ASSERT_VEC_NEAR(rotated[0], glm::vec2(10.0f, 20.0f), 1e-2f);
}

COOPA_TEST(clamp_inside_translates_without_resizing) {
    const Rect bounds{ {0.0f, 0.0f}, {100.0f, 100.0f} };

    const Rect off_high = clamp_inside(bounds, Rect{ {95.0f, 95.0f}, {115.0f, 105.0f} });
    ASSERT_VEC_NEAR(off_high.size(), glm::vec2(20.0f, 10.0f), 1e-4f);
    ASSERT_VEC_NEAR(off_high.min, glm::vec2(80.0f, 90.0f), 1e-4f);

    const Rect off_low = clamp_inside(bounds, Rect{ {-30.0f, -5.0f}, {-10.0f, 5.0f} });
    ASSERT_VEC_NEAR(off_low.min, glm::vec2(0.0f, 0.0f), 1e-4f);

    // Already inside: untouched.
    const Rect inside{ {10.0f, 10.0f}, {20.0f, 20.0f} };
    const Rect same = clamp_inside(bounds, inside);
    ASSERT_VEC_NEAR(same.min, inside.min, 1e-4f);

    // Larger than the bounds on an axis: pinned to the low edge rather than pushed off the
    // far one, so an oversized bubble still shows its start.
    const Rect huge = clamp_inside(bounds, Rect{ {50.0f, 0.0f}, {250.0f, 10.0f} });
    ASSERT_NEAR(huge.min.x, 0.0f, 1e-4f);
}
