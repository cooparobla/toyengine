/**
 * @file imm_widgets_test.cpp
 * @brief uicoopa's immediate-mode widgets as the editor and hub use them, headless: a click clicks once,
 * a text field commits on Enter, a drag edits a float, popups and dropdowns take the click and
 * close on a second click, menus and tree rows route clicks, and tooltips wait their delay every
 * time.
 */

#include <coopa/testing/test.h>

#include <string>

#include "editor/support/imm_harness.h"
#include "editor/support/view_helpers.h"

COOPA_TEST_SUITE("imm_widgets");

namespace toy::editor::testing {

COOPA_TEST(a_press_and_release_clicks_once) {
    ImmHarness h;
    int clicks = 0;
    bool check = false;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        if (c.button("Press", 100)) ++clicks;
        c.checkbox("Check", &check);
        c.end_region();
    };
    h.frame(ui);
    h.click({20, 15}, ui);
    expect(clicks == 1, "a press + release over the button clicks it once");
    h.click({20, 40}, ui);
    expect(check, "clicking the checkbox toggles it");
    h.click({250, 250}, ui);
    expect(clicks == 1 && check, "clicking empty space does nothing");
}

COOPA_TEST(text_input_commits_on_enter) {
    ImmHarness h;
    std::string value = "abc";
    bool committed = false;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        committed |= c.input_text("Name", &value);
        c.end_region();
    };
    h.frame(ui);
    h.click({300, 15}, ui);                          // focus (selects all)
    expect(h.ctx.wants_keyboard(), "clicking a field focuses it");
    h.frame(ui, {'x', 'y', 'z'});
    expect(value == "abc", "typing edits a private buffer, not the value");
    coopa::input::KeyEvent enter{coopa::input::Key::Enter, 0, coopa::input::KeyAction::Press, coopa::input::Mods::None};
    h.frame(ui, {}, {enter});
    expect(committed && value == "xyz", "Enter commits the edit (got '" + value + "')");
    expect(!h.ctx.wants_keyboard(), "and releases the keyboard");
}

COOPA_TEST(drag_float_and_popups_take_the_click) {
    ImmHarness h;
    float v = 1.0f;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        c.drag_float("Value", &v, 0.1f);
        c.end_region();
    };
    h.frame(ui);
    h.mouse = {300, 15};
    h.frame(ui);
    h.down = true;
    h.frame(ui);
    for (int i = 0; i < 5; ++i) { h.mouse.x += 10; h.frame(ui, {}, {}, {10, 0}); }
    h.down = false;
    h.frame(ui);
    expect(std::abs(v - 6.0f) < 1e-3f, "dragging 50 px at 0.1/px adds 5 to 1 (got " + std::to_string(v) + ")");

    // A popup over a button: clicks land on the popup, not the button underneath.
    ImmHarness p;
    int under = 0, item = 0;
    auto ui2 = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        if (c.button("Open", 100)) c.open_popup("menu", glm::vec2(6, 6));
        if (c.button("Under", 100)) ++under;
        if (c.begin_popup("menu", 200)) {
            if (c.menu_item("Item")) ++item;
            c.end_popup();
        }
        c.end_region();
    };
    p.frame(ui2);
    p.click({20, 15}, ui2);
    p.frame(ui2);
    expect(p.ctx.any_popup_open(), "the popup opened");
    p.click({40, 16}, ui2);
    expect(item == 1 && under == 0, "a click inside the popup hits its item, not the widget below");
    expect(!p.ctx.any_popup_open(), "choosing an item closes the popup");
}

/** @brief Clicking an open dropdown's opener closes it (instead of re-opening it). */
COOPA_TEST(clicking_an_open_dropdown_closes_it) {
    ImmHarness h;
    int idx = 0;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.combo_box("dd", {10, 10, 120, 20}, &idx, {"One", "Two", "Three"});
    };
    h.frame(ui);
    h.click({40, 20}, ui);
    h.frame(ui);
    expect(h.ctx.any_popup_open(), "clicking a dropdown opens it");
    h.click({40, 20}, ui);
    h.frame(ui);
    expect(!h.ctx.any_popup_open(), "clicking the open dropdown closes it");
    h.click({40, 20}, ui);
    h.frame(ui);
    expect(h.ctx.any_popup_open(), "and the next click opens it again");

    // A plain button that opens a popup below itself behaves the same.
    ImmHarness p;
    auto ui2 = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 300}, false);
        if (c.button("Open", 100)) c.open_popup("menu", glm::vec2(6, 40));
        if (c.begin_popup("menu", 200)) { c.menu_item("Item"); c.end_popup(); }
        c.end_region();
    };
    p.frame(ui2);
    p.click({20, 15}, ui2);
    p.frame(ui2);
    expect(p.ctx.any_popup_open(), "button popup opened");
    p.click({20, 15}, ui2);
    p.frame(ui2);
    expect(!p.ctx.any_popup_open(), "clicking its opener again closes it");
    // Right-clicking elsewhere while open still closes it (and re-opening via another click works).
    p.click({20, 15}, ui2);
    p.frame(ui2);
    p.click({300, 250}, ui2);
    expect(!p.ctx.any_popup_open(), "clicking empty space closes it");
}

COOPA_TEST(menubar_items_and_tree_rows_route_clicks) {
    ImmHarness h;
    int saved = 0;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_menubar({0, 0, 800, 24});
        if (c.begin_menu("File")) {
            if (c.menu_item("Save", "Ctrl+S")) ++saved;
            c.end_menu();
        }
        c.end_menubar();
    };
    h.frame(ui);
    h.click({15, 12}, ui);
    h.frame(ui);
    expect(h.ctx.any_popup_open(), "clicking a menu header opens it");
    h.click({40, 36}, ui);
    expect(saved == 1, "clicking the item runs it");

    ImmHarness t;
    int clicked = 0;
    bool opened = false;
    auto tree = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        auto r = c.tree_node(c.get_id("root"), "Root", false, false, false);
        if (r.clicked) ++clicked;
        if (r.open) { opened = true; c.tree_node(c.get_id("child"), "Child", true, false); c.tree_pop(); }
        c.end_region();
    };
    t.frame(tree);
    t.click({100, 15}, tree);
    expect(clicked == 1 && !opened, "clicking the row selects without expanding");
    t.click({12, 15}, tree);
    t.frame(tree);
    expect(opened, "clicking the arrow expands");
}

COOPA_TEST(tooltips_wait_their_delay_every_time) {
    ImmHarness h;
    // Two rows with the SAME id (a loop without push_id), plus a plain button.
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        c.button("same");
        c.tooltip("first");
        c.button("same");
        c.tooltip("second");
        c.end_region();
    };
    const float delay = h.ctx.style.tooltip_delay;
    expect(delay >= 0.3f, "tooltips wait a real hover delay (" + std::to_string(delay) + " s)");
    auto frames_until_tip = [&](glm::vec2 at, int max_frames) {
        h.mouse = at;
        for (int i = 1; i <= max_frames; ++i) {
            h.frame(ui);
            if (!h.ctx.tooltip_text().empty()) return i;
        }
        return -1;
    };
    const glm::vec2 first{20, 8}, second{20, 8 + h.ctx.style.row_height + h.ctx.style.spacing};
    const int wait = static_cast<int>(delay * 60.0f);
    const int n = frames_until_tip(first, wait * 3);
    expect(n >= wait, "the first hover waits the delay (" + std::to_string(n) + " frames)");
    // Leave, then come back: the delay applies again (it used to show instantly).
    h.mouse = {250, 250};
    h.frame(ui);
    expect(h.ctx.tooltip_text().empty(), "moving away hides the tip");
    const int again = frames_until_tip(first, wait * 3);
    expect(again >= wait, "returning to the item waits again (" + std::to_string(again) + " frames)");
    // Straight onto another item with the same id: still waits.
    const int other = frames_until_tip(second, wait * 3);
    expect(other >= wait, "a same-id neighbour waits again (" + std::to_string(other) + " frames)");
    expect(h.ctx.tooltip_text() == "second", "and shows its own text");
    // A press hides it until the mouse rests again.
    h.down = true;
    h.frame(ui);
    expect(h.ctx.tooltip_text().empty(), "a press hides the tip");
    h.down = false;
    h.frame(ui);
    expect(h.ctx.tooltip_text().empty(), "and it does not come straight back");
}

} // namespace toy::editor::testing
