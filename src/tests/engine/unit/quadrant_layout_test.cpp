/**
 * @file quadrant_layout_test.cpp
 * @brief Every open-mask of the four-quadrant workspace against quadrant_layout.h's documented
 *        table, plus nearest_slot() at quadrant centres and the area's edges.
 *
 * Headless -- pure arithmetic over Box, no GPU, no font, no Context.
 */

#include <coopa/testing/test.h>

#include <array>
#include <cmath>
#include <string>

#include <toyengine/ui/quadrant_layout.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("quadrant_layout");

namespace {
using coopa::ui::imm::Box;
using toy::ui::Slot;

constexpr Box kArea{0.0f, 0.0f, 100.0f, 100.0f};
constexpr int TL = 0, TR = 1, BL = 2, BR = 3;

void expect_box(const Box& got, const Box& want, const std::string& what) {
    const bool ok = std::abs(got.x - want.x) < 1e-4f && std::abs(got.y - want.y) < 1e-4f &&
                    std::abs(got.w - want.w) < 1e-4f && std::abs(got.h - want.h) < 1e-4f;
    expect(ok, what + " (got {" + std::to_string(got.x) + "," + std::to_string(got.y) + "," +
               std::to_string(got.w) + "," + std::to_string(got.h) + "})");
}
}  // namespace

COOPA_TEST(one_open_panel_fills_the_whole_area) {
    for (int i = 0; i < 4; ++i) {
        std::array<bool, 4> open{false, false, false, false};
        open[i] = true;
        const auto boxes = toy::ui::quadrant_layout(kArea, open);
        expect_box(boxes[i], kArea, "a lone panel fills the area");
    }
}

COOPA_TEST(two_in_the_same_row_become_full_height_halves) {
    {
        const auto b = toy::ui::quadrant_layout(kArea, {true, true, false, false});   // TL+TR
        expect_box(b[TL], {0, 0, 50, 100}, "TL full-height left half");
        expect_box(b[TR], {50, 0, 50, 100}, "TR full-height right half");
    }
    {
        const auto b = toy::ui::quadrant_layout(kArea, {false, false, true, true});   // BL+BR
        expect_box(b[BL], {0, 0, 50, 100}, "BL full-height left half");
        expect_box(b[BR], {50, 0, 50, 100}, "BR full-height right half");
    }
}

COOPA_TEST(two_in_the_same_column_become_full_width_halves) {
    {
        const auto b = toy::ui::quadrant_layout(kArea, {true, false, true, false});   // TL+BL
        expect_box(b[TL], {0, 0, 100, 50}, "TL full-width top half");
        expect_box(b[BL], {0, 50, 100, 50}, "BL full-width bottom half");
    }
    {
        const auto b = toy::ui::quadrant_layout(kArea, {false, true, false, true});   // TR+BR
        expect_box(b[TR], {0, 0, 100, 50}, "TR full-width top half");
        expect_box(b[BR], {0, 50, 100, 50}, "BR full-width bottom half");
    }
}

COOPA_TEST(two_diagonal_become_full_width_halves_stacked) {
    {
        const auto b = toy::ui::quadrant_layout(kArea, {true, false, false, true});   // TL+BR
        expect_box(b[TL], {0, 0, 100, 50}, "TL full-width top half");
        expect_box(b[BR], {0, 50, 100, 50}, "BR full-width bottom half");
    }
    {
        const auto b = toy::ui::quadrant_layout(kArea, {false, true, true, false});   // TR+BL
        expect_box(b[TR], {0, 0, 100, 50}, "TR full-width top half");
        expect_box(b[BL], {0, 50, 100, 50}, "BL full-width bottom half");
    }
}

COOPA_TEST(three_open_the_lone_column_member_spans_full_height_others_stay_quadrants) {
    {   // TL closed: BL is alone in the left column.
        const auto b = toy::ui::quadrant_layout(kArea, {false, true, true, true});
        expect_box(b[BL], {0, 0, 50, 100}, "BL spans the full left column");
        expect_box(b[TR], {50, 0, 50, 50}, "TR stays its own quadrant");
        expect_box(b[BR], {50, 50, 50, 50}, "BR stays its own quadrant");
    }
    {   // TR closed: BR is alone in the right column.
        const auto b = toy::ui::quadrant_layout(kArea, {true, false, true, true});
        expect_box(b[BR], {50, 0, 50, 100}, "BR spans the full right column");
        expect_box(b[TL], {0, 0, 50, 50}, "TL stays its own quadrant");
        expect_box(b[BL], {0, 50, 50, 50}, "BL stays its own quadrant");
    }
    {   // BL closed: TL is alone in the left column.
        const auto b = toy::ui::quadrant_layout(kArea, {true, true, false, true});
        expect_box(b[TL], {0, 0, 50, 100}, "TL spans the full left column");
        expect_box(b[TR], {50, 0, 50, 50}, "TR stays its own quadrant");
        expect_box(b[BR], {50, 50, 50, 50}, "BR stays its own quadrant");
    }
    {   // BR closed: TR is alone in the right column.
        const auto b = toy::ui::quadrant_layout(kArea, {true, true, true, false});
        expect_box(b[TR], {50, 0, 50, 100}, "TR spans the full right column");
        expect_box(b[TL], {0, 0, 50, 50}, "TL stays its own quadrant");
        expect_box(b[BL], {0, 50, 50, 50}, "BL stays its own quadrant");
    }
}

COOPA_TEST(four_open_are_four_unchanged_quadrants) {
    const auto b = toy::ui::quadrant_layout(kArea, {true, true, true, true});
    expect_box(b[TL], {0, 0, 50, 50}, "TL quadrant");
    expect_box(b[TR], {50, 0, 50, 50}, "TR quadrant");
    expect_box(b[BL], {0, 50, 50, 50}, "BL quadrant");
    expect_box(b[BR], {50, 50, 50, 50}, "BR quadrant");
}

COOPA_TEST(nearest_slot_at_each_quadrant_centre) {
    expect(toy::ui::nearest_slot(kArea, {25, 25}) == Slot::TopLeft, "TL centre -> TopLeft");
    expect(toy::ui::nearest_slot(kArea, {75, 25}) == Slot::TopRight, "TR centre -> TopRight");
    expect(toy::ui::nearest_slot(kArea, {25, 75}) == Slot::BottomLeft, "BL centre -> BottomLeft");
    expect(toy::ui::nearest_slot(kArea, {75, 75}) == Slot::BottomRight, "BR centre -> BottomRight");
}

COOPA_TEST(nearest_slot_at_the_areas_edges) {
    // Corners sit squarely inside their own quadrant's half of the area.
    expect(toy::ui::nearest_slot(kArea, {1, 1}) == Slot::TopLeft, "top-left corner -> TopLeft");
    expect(toy::ui::nearest_slot(kArea, {99, 1}) == Slot::TopRight, "top-right corner -> TopRight");
    expect(toy::ui::nearest_slot(kArea, {1, 99}) == Slot::BottomLeft, "bottom-left corner -> BottomLeft");
    expect(toy::ui::nearest_slot(kArea, {99, 99}) == Slot::BottomRight, "bottom-right corner -> BottomRight");
    // Just off the exact centre tie-breaks deterministically toward the nearer quadrant.
    expect(toy::ui::nearest_slot(kArea, {49, 1}) == Slot::TopLeft, "just left of top-centre -> TopLeft");
    expect(toy::ui::nearest_slot(kArea, {51, 1}) == Slot::TopRight, "just right of top-centre -> TopRight");
}

COOPA_TEST(a_split_moves_the_dividers_without_changing_which_slots_merge) {
    const toy::ui::QuadrantSplit s{0.75f, 0.25f};
    const std::array<bool, 4> all{true, true, true, true};
    const auto boxes = toy::ui::quadrant_layout(kArea, all, s);
    expect_box(boxes[TL], {0.0f, 0.0f, 75.0f, 25.0f}, "TopLeft takes the larger width, smaller height");
    expect_box(boxes[TR], {75.0f, 0.0f, 25.0f, 25.0f}, "TopRight takes the remainder of the top row");
    expect_box(boxes[BL], {0.0f, 25.0f, 75.0f, 75.0f}, "BottomLeft takes the remainder of the left column");
    expect_box(boxes[BR], {75.0f, 25.0f, 25.0f, 75.0f}, "BottomRight takes both remainders");

    // The merge rules are the table's regardless of where the dividers sit: two in the same
    // row are still two full-height halves, just unequal ones.
    const std::array<bool, 4> top_row{true, true, false, false};
    const auto merged = toy::ui::quadrant_layout(kArea, top_row, s);
    expect_box(merged[TL], {0.0f, 0.0f, 75.0f, 100.0f}, "TopLeft still spans its full column height");
    expect_box(merged[TR], {75.0f, 0.0f, 25.0f, 100.0f}, "TopRight still spans its full column height");
}

COOPA_TEST(a_split_is_clamped_away_from_the_edges) {
    const std::array<bool, 4> all{true, true, true, true};
    const auto squashed = toy::ui::quadrant_layout(kArea, all, {0.0f, 2.0f});
    expect_box(squashed[TL], {0.0f, 0.0f, kArea.w * toy::ui::kMinSplit, kArea.h * toy::ui::kMaxSplit},
               "a divider dragged past an edge stops at the clamp");
    expect(squashed[BR].w > 0.0f && squashed[BR].h > 0.0f, "no panel is ever clamped to nothing");
}

COOPA_TEST(nearest_slot_follows_the_split) {
    // A point at x=60 is in the RIGHT column under an even split, but in the left one once the
    // vertical divider is dragged out to 0.8 -- a drop lands where the panels actually are.
    // y=60 rather than the midpoint: at exactly 50 the point is equidistant from the top and
    // bottom quadrant centres, and which of the two a tie resolves to is not this test's point.
    const glm::vec2 p{60.0f, 60.0f};
    expect(toy::ui::nearest_slot(kArea, p) == Slot::BottomRight, "even split: x=60 is on the right");
    expect(toy::ui::nearest_slot(kArea, p, {0.8f, 0.5f}) == Slot::BottomLeft,
           "divider at 0.8: x=60 is now inside the left column");
}

COOPA_TEST(a_divider_spans_only_what_it_actually_resizes) {
    // Three open, bottom-right closed: the right column is one full-height panel, so the
    // horizontal divider splits the LEFT column alone and must stop at the vertical divider
    // rather than running on under a panel whose size it does not change.
    const std::array<bool, 4> three{true, true, true, false};
    const auto d = toy::ui::quadrant_dividers(kArea, three, {0.5f, 0.5f}, 10.0f);

    expect_box(d.vertical, {45.0f, 0.0f, 10.0f, 100.0f},
               "the vertical divider spans the FULL height: the tall right panel borders both left quadrants");
    expect_box(d.horizontal, {0.0f, 45.0f, 50.0f, 10.0f},
               "the horizontal divider stops at the vertical one instead of crossing the whole area");

    // The same arrangement mirrored -- bottom-LEFT closed, so the tall panel is on the left.
    const auto m = toy::ui::quadrant_dividers(kArea, {true, true, false, true}, {0.5f, 0.5f}, 10.0f);
    expect_box(m.vertical, {45.0f, 0.0f, 10.0f, 100.0f}, "mirrored: the vertical divider is still full height");
    expect_box(m.horizontal, {50.0f, 45.0f, 50.0f, 10.0f},
               "mirrored: the horizontal divider covers the RIGHT column, the split one");
}

COOPA_TEST(all_four_open_gives_both_dividers_their_full_span) {
    const std::array<bool, 4> all{true, true, true, true};
    const auto d = toy::ui::quadrant_dividers(kArea, all, {0.5f, 0.5f}, 10.0f);
    expect_box(d.vertical, {45.0f, 0.0f, 10.0f, 100.0f}, "both rows are split, so the vertical divider is full height");
    expect_box(d.horizontal, {0.0f, 45.0f, 100.0f, 10.0f}, "both columns are split, so the horizontal divider is full width");
}

COOPA_TEST(a_merged_arrangement_offers_only_the_dividers_that_exist) {
    // Two open in the same row: they are two full-height halves, so there is a vertical divider
    // between them and no horizontal one -- nothing is stacked.
    const auto row = toy::ui::quadrant_dividers(kArea, {true, true, false, false});
    expect(!row.vertical.empty(), "two side by side have a vertical divider");
    expect(row.horizontal.empty(), "two side by side have nothing to divide top from bottom");

    // Two stacked: the mirror image.
    const auto col = toy::ui::quadrant_dividers(kArea, {true, false, true, false});
    expect(col.vertical.empty(), "two stacked have nothing to divide left from right");
    expect(!col.horizontal.empty(), "two stacked have a horizontal divider");

    // A single panel fills the area; there is nothing to resize against.
    const auto one = toy::ui::quadrant_dividers(kArea, {true, false, false, false});
    expect(one.vertical.empty() && one.horizontal.empty(), "a lone panel has no dividers at all");
}
