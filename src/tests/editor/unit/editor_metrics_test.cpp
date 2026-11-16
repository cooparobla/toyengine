/**
 * @file editor_metrics_test.cpp
 * @brief The editor's chrome arithmetic (editor/app/ui/metrics.h): the modal "category list
 *        beside a pane" split, and how a workspace splitter clamps.
 */

#include <coopa/testing/test.h>

#include <string>

#include "editor/app/ui/metrics.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("editor_metrics");

namespace toy::editor::testing {

COOPA_TEST(a_list_pane_split_fills_its_body_exactly) {
    const ui::imm::Box body{10, 20, 600, 400};
    const ui::ListPane lp = ui::list_pane(body, 170.0f, 6.0f);
    expect(lp.list.x == body.x && lp.list.y == body.y, "the list starts at the body's corner");
    expect(lp.list.w == 170.0f, "the list takes the width asked for");
    expect(lp.pane.x == lp.list.right() + 6.0f, "the pane starts one gap past it");
    expect(lp.pane.right() == body.right(), "and reaches the body's far edge");
    expect(lp.list.h == body.h && lp.pane.h == body.h, "both are full height");
}

COOPA_TEST(a_narrow_body_does_not_give_the_pane_a_negative_width) {
    const ui::ListPane lp = ui::list_pane({0, 0, 100, 200}, 170.0f, 6.0f);
    expect(lp.pane.w >= 0.0f, "the pane's width never goes negative (got " + std::to_string(lp.pane.w) + ")");
    expect(lp.list.w <= 100.0f, "and the list does not overrun the body");
}

COOPA_TEST(a_splitter_clamps_to_a_floor_and_a_fraction) {
    expect(ui::clamp_split(50.0f, 200.0f, 2000.0f, 0.35f) == 200.0f, "below the floor it snaps up to it");
    expect(ui::clamp_split(300.0f, 200.0f, 2000.0f, 0.35f) == 300.0f, "inside the range it is kept");
    expect(ui::clamp_split(900.0f, 200.0f, 2000.0f, 0.35f) == 700.0f, "above the ceiling it snaps down to it");
    // A window so small that the fraction falls under the floor: the floor still wins, so the
    // area stays usable rather than collapsing.
    expect(ui::clamp_split(300.0f, 200.0f, 300.0f, 0.35f) == 200.0f,
           "a tiny window keeps the floor rather than the fraction");
}

} // namespace toy::editor::testing
