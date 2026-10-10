/**
 * @file undo_stack_test.cpp
 * @brief UndoStack: sequence numbers order steps across the editor's several stacks (one Ctrl+Z timeline),
 * a push anywhere invalidates every redo, and the memory budget trims the oldest steps.
 */

#include <coopa/testing/test.h>

#include <vector>

#include "editor/core/undo.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("undo_stack");

namespace toy::editor::testing {

/** @brief UndoStack's cross-stack ordering, redo freshness and memory budget. */
COOPA_TEST(steps_order_across_stacks_and_the_budget_trims_the_oldest) {
    UndoStack<int> a, b;
    a.push("a1", 0, 1);
    b.push("b1", 0, 1);
    a.push("a2", 1, 2);
    expect(a.top_undo_seq() > b.top_undo_seq() && b.top_undo_seq() > 0, "sequence numbers order steps across stacks");
    a.undo();
    b.undo();
    expect(a.redo_fresh() && b.redo_fresh(), "redo stays valid while nothing new is pushed");
    expect(b.top_redo_seq() < a.top_redo_seq(), "the step undone last is the older one (redo it first)");
    UndoStack<int> c;
    c.push("c1", 0, 1);
    expect(!a.redo_fresh() && !b.redo_fresh(), "a push anywhere invalidates every other stack's redo");

    UndoStack<std::vector<int>> big;
    big.set_budget(1000, [](const std::vector<int>& v) { return v.size() * sizeof(int); }, 3);
    for (int i = 0; i < 20; ++i) big.push("step", std::vector<int>(50), std::vector<int>(50));   // 400 B per step
    expect(big.undo_count() == 3, "a memory budget trims the oldest steps, keeping the minimum (" +
                                      std::to_string(big.undo_count()) + ")");
    expect(big.bytes() == 3u * 400u, "...and tracks what is left");
}

} // namespace toy::editor::testing
