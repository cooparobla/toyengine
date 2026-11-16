/**
 * @file imm_widgets_test.cpp
 * @brief uicoopa's immediate-mode widgets as the editor and hub use them, headless: a click clicks once,
 * a text field commits on Enter, a drag edits a float, popups and dropdowns take the click and
 * close on a second click, menus and tree rows route clicks, and tooltips wait their delay every
 * time.
 */

#include <coopa/testing/test.h>

#include <string>
#include <utility>
#include <vector>

#include <uicoopa/immediate/imm_icons.h>

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

/** @brief A segmented picker: clicking a cell selects it, clicking the one already on does nothing. */
COOPA_TEST(a_segmented_picker_selects_the_cell_clicked) {
    ImmHarness h;
    int index = 0;
    int changes = 0;
    const std::vector<std::string> items = {"Inline", "Asset", "Asset +"};
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 100}, false);
        if (c.segmented("Material", &index, items)) ++changes;
        c.end_region();
    };
    h.frame(ui);
    // The widget column starts at label_ratio of the row; three cells share what is left.
    const float x0 = 400.0f * h.ctx.style.label_ratio;
    const float cell = (400.0f - x0) / 3.0f;
    h.click({x0 + cell * 2.5f, 10}, ui);
    expect(index == 2 && changes == 1, "the third cell is picked (index " + std::to_string(index) + ")");
    h.click({x0 + cell * 2.5f, 10}, ui);
    expect(index == 2 && changes == 1, "clicking the cell already on reports no change");
    h.click({x0 + cell * 0.5f, 10}, ui);
    expect(index == 0 && changes == 2, "and the first cell picks again");
}

/** @brief A range slider: each handle drags, and neither can cross the other. */
COOPA_TEST(a_range_slider_keeps_its_low_below_its_high) {
    ImmHarness h;
    float lo = 2.0f, hi = 8.0f;
    auto ui = [&](coopa::ui::imm::Context& c) {
        c.begin_region("r", {0, 0, 400, 100}, false);
        c.range_slider("Lifetime", &lo, &hi, 0.0f, 10.0f);
        c.end_region();
    };
    h.frame(ui);
    const float x0 = 400.0f * h.ctx.style.label_ratio;
    const float w = 400.0f - x0;
    auto at = [&](float v) { return x0 + w * (v / 10.0f); };

    // Grab the low handle where it is, drag it right past the high one.
    h.mouse = {at(2.0f), 10};
    h.frame(ui);
    h.down = true;
    h.frame(ui);
    h.mouse = {at(9.5f), 10};
    h.frame(ui);
    expect(lo <= hi + 1e-4f, "the low handle stops at the high one (" + std::to_string(lo) + " vs " + std::to_string(hi) + ")");
    expect(lo > 2.0f, "...having moved toward it");
    h.down = false;
    h.frame(ui);

    const float was_lo = lo;
    h.mouse = {at(hi), 10};
    h.frame(ui);
    h.down = true;
    h.frame(ui);
    h.mouse = {at(0.5f), 10};
    h.frame(ui);
    expect(hi >= was_lo - 1e-4f, "and the high handle stops at the low one");
    h.down = false;
    h.frame(ui);
}

COOPA_TEST(a_reorder_grip_steps_one_row_per_pitch_of_travel) {
    namespace imm = coopa::ui::imm;
    ImmHarness h;
    std::vector<std::string> items{"a", "b", "c"};
    const float row_h = 20.0f, pitch = 23.0f;
    auto ui = [&](imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        const imm::Id list = c.get_id("items");
        imm::ReorderRequest rq;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const imm::Box grip{0, static_cast<float>(i) * pitch, row_h, row_h};
            if (auto q = c.reorder_grip(list, grip, i, static_cast<int>(items.size()), pitch); q.wanted()) rq = q;
        }
        c.reorder_release(list);
        if (rq.wanted()) {
            std::swap(items[static_cast<size_t>(rq.from)], items[static_cast<size_t>(rq.to)]);
            c.reorder_step(list, rq.to);
        }
        c.end_region();
    };

    h.mouse = {10, 10};                 // over the first row's grip
    h.frame(ui);
    h.down = true;
    h.frame(ui);
    expect(items[0] == "a", "the press itself does not move anything");
    expect(h.ctx.reorder_dragging(h.ctx.get_id("items"), 0) ||
           items[0] == "a", "the first row is the one being dragged");

    h.mouse.y = 10 + pitch;             // one row of travel
    h.frame(ui);
    expect(items[0] == "b" && items[1] == "a", "one pitch down swaps with the next row");

    h.mouse.y = 10 + pitch * 2;         // and the anchor followed, so it steps again
    h.frame(ui);
    expect(items[1] == "c" && items[2] == "a",
           "a held drag keeps stepping (got " + items[0] + items[1] + items[2] + ")");

    h.mouse.y = 10 + pitch * 3;         // past the end
    h.frame(ui);
    expect(items[2] == "a", "a drag past the last row asks for nothing");

    h.down = false;
    h.frame(ui);
    expect(!h.ctx.reorder_dragging(h.ctx.get_id("items"), 2), "releasing the mouse ends the drag");
}

COOPA_TEST(a_grip_drag_past_the_first_row_asks_for_nothing) {
    namespace imm = coopa::ui::imm;
    ImmHarness h;
    std::vector<int> items{1, 2};
    const float pitch = 23.0f;
    auto ui = [&](imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        const imm::Id list = c.get_id("items");
        imm::ReorderRequest rq;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const imm::Box grip{0, static_cast<float>(i) * pitch, 20, 20};
            if (auto q = c.reorder_grip(list, grip, i, static_cast<int>(items.size()), pitch); q.wanted()) rq = q;
        }
        c.reorder_release(list);
        if (rq.wanted()) {
            std::swap(items[static_cast<size_t>(rq.from)], items[static_cast<size_t>(rq.to)]);
            c.reorder_step(list, rq.to);
        }
        c.end_region();
    };
    h.mouse = {10, 10};
    h.frame(ui);
    h.down = true;
    h.frame(ui);
    h.mouse.y = 10 - pitch * 2;     // dragging the FIRST row upwards
    h.frame(ui);
    expect(items[0] == 1 && items[1] == 2, "the first row cannot be moved above itself");
    h.down = false;
    h.frame(ui);
}

COOPA_TEST(every_icon_has_a_name_that_maps_back_to_it) {
    namespace imm = coopa::ui::imm;
    int checked = 0;
    for (int i = 1; i < static_cast<int>(imm::Icon::Count); ++i) {
        const imm::Icon ic = static_cast<imm::Icon>(i);
        const char* name = imm::icon_name(ic);
        expect(name[0] != '\0', "icon " + std::to_string(i) + " has a name");
        expect(imm::icon_from_name(name) == ic,
               std::string("'") + name + "' maps back to the icon it names");
        ++checked;
    }
    expect(checked > 100, "the whole set was checked (" + std::to_string(checked) + ")");
    expect(imm::icon_from_name("grip") == imm::Icon::Grip, "a name still matches without regard to case");
    expect(imm::icon_from_name("nonesuch") == imm::Icon::None, "an unknown name is Icon::None");
}

COOPA_TEST(subhead_chip_and_primary_button_draw) {
    namespace imm = coopa::ui::imm;
    ImmHarness h;
    int clicks = 0, disabled_clicks = 0;
    imm::Box chip_rect{}, button_rect{};
    auto ui = [&](imm::Context& c) {
        c.begin_region("r", {0, 0, 300, 300}, false);
        c.subhead("Group");
        chip_rect = c.chip("tag");
        if (c.primary_button("Build", 120)) ++clicks;
        button_rect = c.last_rect();
        if (c.primary_button("Cancel", 120, false)) ++disabled_clicks;
        c.end_region();
    };
    h.frame(ui);
    expect(chip_rect.w >= h.ctx.text_width("tag"), "a chip is at least as wide as its text");
    expect(chip_rect.h > 0 && button_rect.h > 0, "both drew a box");

    h.click(button_rect.center(), ui);
    expect(clicks == 1, "the primary button clicks once");
    h.frame(ui);
    h.click({button_rect.center().x, button_rect.center().y + button_rect.h + 3}, ui);
    expect(disabled_clicks == 0, "a disabled primary button does not click");
}

} // namespace toy::editor::testing
