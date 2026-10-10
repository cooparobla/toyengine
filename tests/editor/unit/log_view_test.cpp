/**
 * @file log_view_test.cpp
 * @brief The hub's log drawer and the editor's build window: the newest line stays in view while output
 * streams in, unless the user scrolls up; back at the bottom, it follows again.
 */

#include <coopa/testing/test.h>

#include "editor/support/imm_harness.h"
#include "editor/ui/log_view.h"

COOPA_TEST_SUITE("log_view");

namespace toy::editor::testing {

COOPA_TEST(follows_output_until_scrolled_up) {
    // The hub's log drawer / the editor's build window: newest line in view while output
    // streams in, unless the user scrolls up; back at the bottom, it follows again.
    ImmHarness h;
    toy::editor::LogView view;
    std::vector<std::string> lines;
    const coopa::ui::imm::Box box{10, 10, 400, 168};   // 10 lines of 16 px
    auto ui = [&](coopa::ui::imm::Context& c) { view.draw(c, box, lines); };
    h.mouse = {100, 50};
    for (int i = 0; i < 40; ++i) { lines.push_back("line " + std::to_string(i)); h.frame(ui); }
    expect(view.following() && view.scroll() == 30, "it shows the newest lines while output streams in (" + std::to_string(view.scroll()) + ")");
    h.scroll = {0, 2};   // wheel up
    h.frame(ui);
    const int held = view.scroll();
    expect(!view.following() && held < 30, "scrolling up stops following");
    for (int i = 0; i < 10; ++i) { lines.push_back("more"); h.frame(ui); }
    expect(view.scroll() == held, "...and new output doesn't move the view");
    for (int i = 0; i < 10; ++i) { h.scroll = {0, -2}; h.frame(ui); }
    expect(view.following() && view.scroll() == 40, "scrolling back to the bottom follows again");
    lines.push_back("last");
    h.frame(ui);
    expect(view.scroll() == 41, "...so the next line is in view");
}

} // namespace toy::editor::testing
