/**
 * @file imm_game_test.cpp
 * @brief The immediate layer's game-facing surface: anchored placement, the composites in
 *        imm_game.h, the node-geometry probe an editor reads, and world-space hosting.
 *
 * Headless and font-free. Without a Font, imm::TextRenderer measures every glyph at 0.55 em
 * and draws nothing, so layout and input logic are fully exercisable with no GPU -- which is
 * the whole reason game UI moving to this layer makes it testable at all.
 */

#include <coopa/testing/test.h>

#include <uicoopa/immediate/imm.h>
#include <coopa/input/keys.h>
#include <uicoopa/immediate/imm_canvas.h>

#include <toyengine/ui/imm_screen.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/input/input.h>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string>
#include <vector>

COOPA_TEST_SUITE("imm_game");

namespace {

using coopa::ui::imm::Anchor;
using coopa::ui::imm::Box;
using coopa::ui::imm::Context;
using coopa::ui::imm::FrameInput;

/** @brief Drives one imm frame over a scratch DrawList at a fixed canvas size. */
struct Harness {
    Context ctx;
    coopa::ui::DrawList dl;
    glm::vec2 size{800.0f, 600.0f};
    glm::vec2 mouse{-1.0e6f};
    bool down = false;
    bool prev_down = false;
    float dt = 1.0f / 60.0f;

    /// Keys delivered on the NEXT frame() only, the way a real frame's event queue behaves.
    std::vector<coopa::input::KeyEvent> keys;

    void press_key(coopa::input::Key k, coopa::input::Mods mods = coopa::input::Mods::None) {
        keys.push_back({k, 0, coopa::input::KeyAction::Press, mods});
    }

    void frame(const std::function<void(Context&)>& fn) {
        FrameInput in;
        in.mouse = mouse;
        in.dt = dt;
        in.down[0] = down;
        in.pressed[0] = down && !prev_down;
        in.released[0] = !down && prev_down;
        in.keys = keys;
        keys.clear();
        prev_down = down;
        dl.begin(coopa::ui::Rect{glm::vec2(0.0f), size});
        ctx.begin_frame(dl, in, size);
        fn(ctx);
        ctx.end_frame();
    }

    /// Press and release over `at`, which takes two frames: imm resolves a click on release,
    /// and its hover layer comes from the PREVIOUS frame's rects.
    void click(glm::vec2 at, const std::function<void(Context&)>& fn) {
        mouse = at;
        frame(fn);          // hover established
        down = true;
        frame(fn);          // press
        down = false;
        frame(fn);          // release -> click
    }
};

}  // namespace

// =====================================================================================
// Anchored placement
// =====================================================================================

COOPA_TEST(anchor_pins_each_corner_and_offsets_away_from_its_edge) {
    Harness h;
    h.frame([&](Context& c) {
        const glm::vec2 sz(260.0f, 28.0f);

        // Top-left is the origin, so the offset is the position outright.
        const Box tl = c.anchor(Anchor::TopLeft, {20.0f, 20.0f}, sz);
        ASSERT_NEAR(tl.x, 20.0f, 1e-4f);
        ASSERT_NEAR(tl.y, 20.0f, 1e-4f);

        // Top-right pins the box's RIGHT edge to the canvas's. A positive x offset would push
        // it off-screen, so an author insets with a negative one -- the offset always moves
        // the box, never the anchor.
        const Box tr = c.anchor(Anchor::TopRight, {-20.0f, 20.0f}, sz);
        ASSERT_NEAR(tr.right(), h.size.x - 20.0f, 1e-4f);
        ASSERT_NEAR(tr.y, 20.0f, 1e-4f);

        // Bottom-centre with a negative y floats above the bottom edge: the shape a hotbar
        // tunable takes (`pos: [0, -40]`).
        const Box bc = c.anchor(Anchor::BottomCenter, {0.0f, -40.0f}, sz);
        ASSERT_NEAR(bc.x + bc.w * 0.5f, h.size.x * 0.5f, 1e-4f);
        ASSERT_NEAR(bc.bottom(), h.size.y - 40.0f, 1e-4f);

        // Centre is centred on both axes with no offset.
        const Box mid = c.anchor(Anchor::Center, {0.0f, 0.0f}, sz);
        ASSERT_VEC_NEAR(mid.center(), h.size * 0.5f, 1e-4f);
    });
}

COOPA_TEST(stretch_insets_and_edge_relative_bands) {
    Harness h;
    h.frame([&](Context& c) {
        const Box all = c.stretch(12.0f);
        ASSERT_NEAR(all.x, 12.0f, 1e-4f);
        ASSERT_NEAR(all.w, h.size.x - 24.0f, 1e-4f);
        ASSERT_NEAR(all.h, h.size.y - 24.0f, 1e-4f);

        // A footer: negative y measures up from the bottom, so the caller does not have to
        // know the canvas height.
        const Box footer = c.stretch_h(-48.0f, 40.0f);
        ASSERT_NEAR(footer.y, h.size.y - 48.0f, 1e-4f);
        ASSERT_NEAR(footer.w, h.size.x, 1e-4f);

        // A right-hand sidebar, likewise.
        const Box side = c.stretch_v(-240.0f, 240.0f);
        ASSERT_NEAR(side.x, h.size.x - 240.0f, 1e-4f);
        ASSERT_NEAR(side.right(), h.size.x, 1e-4f);

        // A degenerate margin clamps to zero rather than going negative.
        const Box squashed = c.stretch(glm::vec2(1000.0f));
        ASSERT_NEAR(squashed.w, 0.0f, 1e-4f);
        ASSERT_NEAR(squashed.h, 0.0f, 1e-4f);
    });
}

// =====================================================================================
// Composites
// =====================================================================================

COOPA_TEST(stat_bar_damage_trail_holds_then_closes) {
    Harness h;
    const Box b{0.0f, 0.0f, 200.0f, 20.0f};
    coopa::ui::imm::BarStyle style;
    style.trail_hold = 0.1f;
    style.trail_rate = 1.0f;   // the whole bar per second

    // Settles at full on the first frame rather than animating in from empty: a bar that
    // started at zero would play a fill-up every time a screen opened.
    float first = -1.0f;
    h.frame([&](Context& c) {
        c.stat_bar("hp", b, "Health", 100.0f, 100.0f, style);
        c.push_id("hp");
        first = c.persistent("trail", 0);
        c.pop_id();
    });
    ASSERT_NEAR(first, 1.0f, 1e-4f);

    // Drop to half. The trail must stay at the OLD value through the hold, which is what
    // makes a hit readable; a trail that tracked the fill would be invisible.
    h.dt = 0.05f;
    h.frame([&](Context& c) { c.stat_bar("hp", b, "Health", 50.0f, 100.0f, style); });
    float trail_after_hit = 0.0f;
    h.frame([&](Context& c) {
        c.stat_bar("hp", b, "Health", 50.0f, 100.0f, style);
        c.push_id("hp");
        trail_after_hit = c.persistent("trail", 0);
        c.pop_id();
    });
    ASSERT_TRUE(trail_after_hit > 0.9f);

    // Past the hold, it closes toward the value and never overshoots below it.
    h.dt = 0.2f;
    for (int i = 0; i < 20; ++i) {
        h.frame([&](Context& c) { c.stat_bar("hp", b, "Health", 50.0f, 100.0f, style); });
    }
    float settled = 0.0f;
    h.frame([&](Context& c) {
        c.stat_bar("hp", b, "Health", 50.0f, 100.0f, style);
        c.push_id("hp");
        settled = c.persistent("trail", 0);
        c.pop_id();
    });
    ASSERT_NEAR(settled, 0.5f, 1e-3f);
}

COOPA_TEST(stat_bar_survives_a_zero_maximum) {
    Harness h;
    // A creature whose max health has not been set yet must not divide by it.
    h.frame([&](Context& c) {
        c.stat_bar("hp", {0.0f, 0.0f, 100.0f, 16.0f}, "Health", 5.0f, 0.0f);
    });
    ASSERT_TRUE(h.dl.vertices().size() > 0);   // drew the track, and did not produce NaNs
    for (const coopa::ui::UiVertex& v : h.dl.vertices()) {
        ASSERT_TRUE(std::isfinite(v.x) && std::isfinite(v.y));
    }
}

COOPA_TEST(slot_grid_divides_its_box_exactly) {
    Harness h;
    std::vector<Box> cells;
    coopa::ui::imm::SlotStyle style;
    style.spacing = 4.0f;
    const Box b{10.0f, 10.0f, 200.0f, 100.0f};
    h.frame([&](Context& c) {
        c.slot_grid("bag", b, 4, 2, [&](Context&, const coopa::ui::imm::SlotInfo& s) {
            cells.push_back(s.box);
        }, style);
    });

    ASSERT_EQ(cells.size(), size_t(8));
    // Row-major from the top-left, and the last cell's far corner lands exactly on the box's
    // -- a grid that left a ragged margin would read as misaligned against its neighbours.
    ASSERT_VEC_NEAR(cells.front().pos(), b.pos(), 1e-4f);
    ASSERT_NEAR(cells.back().right(), b.right(), 1e-3f);
    ASSERT_NEAR(cells.back().bottom(), b.bottom(), 1e-3f);
}

COOPA_TEST(slot_grid_reports_the_cell_that_was_clicked) {
    Harness h;
    const Box b{0.0f, 0.0f, 400.0f, 200.0f};
    int clicked = -1;
    auto draw = [&](Context& c) {
        c.slot_grid("bag", b, 4, 2, [&](Context&, const coopa::ui::imm::SlotInfo& s) {
            if (s.pressed) clicked = s.index;
        });
    };
    // Cell 5 is row 1, column 1 -- its centre is a quarter in and three quarters down.
    h.click({b.w * 0.375f, b.h * 0.75f}, draw);
    ASSERT_EQ(clicked, 5);
}

COOPA_TEST(menu_list_reports_the_pressed_row_and_ignores_disabled_ones) {
    Harness h;
    const std::vector<std::string> items{"Resume", "Settings", "Quit"};
    const Box b{0.0f, 0.0f, 200.0f, 150.0f};

    int picked = -2;
    h.click({100.0f, 75.0f}, [&](Context& c) { picked = c.menu_list("m", b, items); });
    ASSERT_EQ(picked, 1);   // the middle row

    // Disabled rows draw but do not report, so a greyed "Resume" cannot be activated.
    const std::vector<bool> off{false, true, false};
    int blocked = -2;
    Harness h2;
    h2.click({100.0f, 75.0f}, [&](Context& c) { blocked = c.menu_list("m", b, items, &off); });
    ASSERT_EQ(blocked, -1);
}

COOPA_TEST(message_log_handles_a_long_history_and_a_box_too_short_for_a_line) {
    // Font-free, so glyph output cannot be counted here -- what this pins down is that the
    // bounds arithmetic holds at both extremes: a history far longer than the box, and a box
    // with no room for even one row. Both used to be where an off-by-one would index past the
    // end of `lines`.
    std::vector<std::string> many;
    for (int i = 0; i < 1000; ++i) many.push_back("line " + std::to_string(i));

    Harness h;
    h.frame([&](Context& c) {
        c.message_log({0.0f, 0.0f, 300.0f, 200.0f}, many, nullptr, false);
        c.message_log({0.0f, 0.0f, 300.0f, 1.0f}, many, nullptr, false);   // shorter than a line
        c.message_log({0.0f, 0.0f, 300.0f, 200.0f}, {}, nullptr, false);   // nothing to show
        // A colour list shorter than the lines must fall back per line, not read past its end.
        const std::vector<glm::vec4> two{glm::vec4(1.0f), glm::vec4(0.5f)};
        c.message_log({0.0f, 0.0f, 300.0f, 200.0f}, many, &two, false);
    });
    for (const coopa::ui::UiVertex& v : h.dl.vertices()) {
        ASSERT_TRUE(std::isfinite(v.x) && std::isfinite(v.y));
    }
}

COOPA_TEST(panel_frame_content_box_sits_below_the_title_bar) {
    Harness h;
    const Box b{20.0f, 20.0f, 300.0f, 200.0f};
    h.frame([&](Context& c) {
        const Box content = c.panel_frame("inv", b, "Inventory");
        ASSERT_TRUE(content.y > b.y);              // below the bar
        ASSERT_TRUE(content.right() <= b.right());  // inside the frame
        ASSERT_TRUE(content.bottom() <= b.bottom());
        ASSERT_TRUE(content.h > 0.0f);

        // An untitled frame has no bar to sit under, so its content is the whole inset box.
        const Box plain = c.panel_frame("plain", {400.0f, 20.0f, 100.0f, 100.0f}, {});
        ASSERT_NEAR(plain.y, 20.0f + c.style.padding, 1e-4f);
    });
}

COOPA_TEST(drag_handle_moves_its_offset_by_the_mouse_delta) {
    Harness h;
    glm::vec2 offset(0.0f);
    const Box b{0.0f, 0.0f, 100.0f, 30.0f};
    auto draw = [&](Context& c) { c.drag_handle("grip", b, &offset); };

    h.mouse = {50.0f, 15.0f};
    h.frame(draw);
    h.down = true;
    h.frame(draw);              // press: becomes active

    // A held handle follows the pointer. The delta is what moves it, not the absolute
    // position, so a handle can be grabbed anywhere on itself.
    FrameInput in;
    in.mouse = glm::vec2(70.0f, 25.0f);
    in.mouse_delta = glm::vec2(20.0f, 10.0f);
    in.down[0] = true;
    in.dt = 1.0f / 60.0f;
    h.dl.begin(coopa::ui::Rect{glm::vec2(0.0f), h.size});
    h.ctx.begin_frame(h.dl, in, h.size);
    draw(h.ctx);
    h.ctx.end_frame();
    ASSERT_VEC_NEAR(offset, glm::vec2(20.0f, 10.0f), 1e-4f);

    // A null target is a no-op rather than a crash: a screen may not have loaded its
    // tunables yet on the first frame.
    h.frame([&](Context& c) { ASSERT_TRUE(!c.drag_handle("grip2", b, nullptr)); });
}


COOPA_TEST(a_string_literal_id_hashes_its_contents_not_its_address) {
    // Without a const char* overload, `get_id("row")` binds to get_id(const void*): array-to-
    // pointer decay is a standard conversion and beats string_view's user-defined one, so a
    // literal silently hashed its own ADDRESS. Sixteen editor call sites pass literals, and
    // the two forms then produced different ids for the same name -- a widget would lose the
    // state it stored last frame the moment one path used a string_view.
    Harness h;
    h.frame([&](Context& c) {
        ASSERT_EQ(c.get_id("row"), c.get_id(std::string_view("row")));
        ASSERT_EQ(c.get_id("row"), c.get_id(std::string("row")));
        // Different names must still differ, i.e. it really is hashing contents.
        ASSERT_TRUE(c.get_id("row") != c.get_id("col"));

        // And the id stack composes the same way through either form.
        c.push_id("outer");
        const coopa::ui::imm::Id via_literal = c.get_id("inner");
        c.pop_id();
        c.push_id(std::string_view("outer"));
        const coopa::ui::imm::Id via_view = c.get_id("inner");
        c.pop_id();
        ASSERT_EQ(via_literal, via_view);
    });

    // The pointer overload is still reachable for an actual pointer, which is what it is for.
    int a = 0;
    int b = 0;
    h.frame([&](Context& c) {
        ASSERT_TRUE(c.get_id(static_cast<const void*>(&a)) != c.get_id(static_cast<const void*>(&b)));
    });
}

COOPA_TEST(persistent_state_is_reachable_through_the_same_id_a_composite_used) {
    // The practical consequence of the overload bug: a composite stores its state under
    // push_id(string_view) and a caller reads it back with a literal. They must agree, or a
    // stat bar's trail is invisible to anything that tries to inspect it.
    Harness h;
    h.frame([&](Context& c) {
        c.push_id(std::string_view("hp"));
        c.persistent("trail", 0) = 0.75f;
        c.pop_id();
    });
    float read_back = -1.0f;
    h.frame([&](Context& c) {
        c.push_id("hp");
        read_back = c.persistent("trail", 0);
        c.pop_id();
    });
    ASSERT_NEAR(read_back, 0.75f, 1e-6f);
}

// =====================================================================================
// Hosting: screen and world
// =====================================================================================

COOPA_TEST(immediate_canvas_takes_its_dt_from_the_scene) {
    // frame_dt went unassigned by every host in the tree for as long as the layer existed,
    // so every tooltip delay and caret blink ran on a fictional 1/60 clock.
    coopa::scene::Scene scene("ui");
    coopa::ui::ImmediateCanvas* ic = coopa::ui::build_immediate_canvas(scene, "UI");
    ASSERT_TRUE(ic != nullptr);

    ASSERT_NEAR(ic->frame_dt, 1.0f / 60.0f, 1e-6f);
    ic->update(0.25f);
    ASSERT_NEAR(ic->frame_dt, 0.25f, 1e-6f);

    // And it reaches the Context, which is the only reason it matters.
    float seen = 0.0f;
    ic->on_draw = [&](Context& c) { seen = c.input().dt; };
    scene.update(0.5f);
    scene.late_update(0.5f);
    ASSERT_NEAR(seen, 0.5f, 1e-6f);
}

COOPA_TEST(clear_emitted_empties_the_draw_list_but_keeps_layout) {
    // How an editor keeps a level's HUD out of the scene view. Simply not previewing would
    // leave the last frame's geometry in the DrawList and the canvas would draw a frozen copy
    // of itself forever, so the clear is the whole point.
    coopa::scene::Scene scene("ui");
    coopa::ui::ImmediateCanvas* ic = coopa::ui::build_immediate_canvas(scene, "UI");
    ic->on_draw = [](Context& c) { c.fill(c.stretch(), glm::vec4(1.0f)); };
    coopa::ui::CanvasComponent* canvas = ic->owner->parent()->get_component<coopa::ui::CanvasComponent>();
    ASSERT_TRUE(canvas != nullptr);
    canvas->set_viewport(800, 600);

    canvas->preview_refresh();
    ASSERT_TRUE(canvas->draw_list().vertices().size() > 0);

    canvas->clear_emitted();
    ASSERT_EQ(canvas->draw_list().vertices().size(), size_t(0));
    // Layout still ran, so an editor can still ask where a widget would be in order to
    // select it -- hiding a UI must not blind the tool that edits it.
    ASSERT_VEC_NEAR(canvas->root_rect().size(), glm::vec2(800.0f, 600.0f), 1e-4f);
}

COOPA_TEST(world_immediate_canvas_hosts_an_imm_pass_and_hit_tests_through_the_ray) {
    coopa::scene::Scene scene("world_ui");
    auto owner = std::make_unique<coopa::scene::SceneObject>("Creature");
    auto* tc = owner->add_component<coopa::scene::TransformComponent>();
    tc->transform().set_position(glm::vec3(0.0f, 0.0f, 1.1f));
    coopa::scene::SceneObject* creature = scene.add_root_object(std::move(owner));
    scene.adopt(*creature);

    const glm::vec2 plate(200.0f, 60.0f);
    coopa::ui::ImmediateCanvas* ic = coopa::ui::build_world_immediate_canvas(
        scene, creature, "Nameplate", plate, 120.0f, 2.0f);
    ASSERT_TRUE(ic != nullptr);

    // The pass is a CHILD of its canvas, which is the only arrangement that draws: a canvas
    // emits its children's components, never its own object's.
    ASSERT_TRUE(ic->owner->parent() != nullptr);
    coopa::ui::CanvasComponent* canvas = ic->owner->parent()->get_component<coopa::ui::CanvasComponent>();
    ASSERT_TRUE(canvas != nullptr);
    ASSERT_TRUE(canvas->is_world_space());
    canvas->rebuild_layout(0, 0);
    ASSERT_VEC_NEAR(canvas->root_rect().size(), plate, 1e-4f);

    // A world canvas emits through the same path; the imm pass sees the plate's own size.
    glm::vec2 seen_size(0.0f);
    bool hovered = false;
    ic->on_draw = [&](Context& c) {
        seen_size = c.canvas_size();
        c.invisible_button("hit", c.stretch(), &hovered);
    };

    // The plate must sit ON the creature, which only holds if build_world_immediate_canvas()
    // linked the transform parent -- add_child() links objects, not transforms. An unlinked
    // plate resolves at the world origin, which reads as a hit 1.1 * 120 = 132 canvas px off.
    ASSERT_NEAR(canvas->owner->get_transform()->get_world_matrix()[3][2], 1.1f, 1e-4f);

    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, -6.0f, 1.1f), glm::vec3(0.0f, 0.0f, 1.1f),
                                       glm::vec3(0.0f, 0.0f, 1.0f));
    canvas->update_world_transform(view);

    // Aim a ray at the plate's centre and feed the canvas-space hit the way a host does.
    const std::optional<glm::vec2> hit =
        canvas->ray_to_canvas(glm::vec3(0.0f, -6.0f, 1.1f), glm::vec3(0.0f, 1.0f, 0.0f));
    ASSERT_TRUE(hit.has_value());
    ASSERT_VEC_NEAR(*hit, plate * 0.5f, 1.0f);

    coopa::input::Input input;
    canvas->set_world_input(input, *hit);
    canvas->preview_refresh();                 // layout + emit, no simulation
    ASSERT_VEC_NEAR(seen_size, plate, 1e-4f);
    canvas->set_world_input(input, *hit);
    canvas->preview_refresh();                 // hover resolves against last frame's layers
    ASSERT_TRUE(hovered);

    // A miss must park far outside, not at (0,0) -- the plate's own corner, which would leave
    // whatever sits there permanently hovered.
    canvas->set_world_input(input, glm::vec2(-1.0e6f));
    canvas->preview_refresh();
    canvas->set_world_input(input, glm::vec2(-1.0e6f));
    canvas->preview_refresh();
    ASSERT_TRUE(!hovered);
}

// =====================================================================================
// ImmScreen: the component shape a game's interface takes
// =====================================================================================

/**
 * @brief Destroying the object a screen lives on takes the canvas with it, and anything still
 *        holding the screen's callback stops rather than calling into freed memory.
 *
 * A screen-space canvas used to be a ROOT object, so destroying the owner left it behind with
 * an `on_draw` that captured a raw `this` -- a pure-virtual call on the next emit, plus one
 * orphan canvas per scene load. The canvas is now a child of the owner, and both the callback
 * and the queued build read through a liveness token the destructor clears.
 */
// =====================================================================================
// Keyboard focus
// =====================================================================================

/**
 * @brief Tab walks the widgets in declaration order, Shift+Tab walks back, and both wrap.
 *
 * Declaration order IS tab order, which is what makes a new widget reachable without anyone
 * having to maintain a list. The order is rebuilt every frame on purpose: a UI whose shape
 * changes between frames gets a tab order that follows it, rather than one captured when it was
 * first drawn.
 */
COOPA_TEST(tab_moves_focus_in_declaration_order_and_wraps) {
    Harness h;
    // Opt-in: a Context with a keymap of its own (the editor) leaves this off.
    h.ctx.keyboard_focus = true;
    auto three = [](Context& c) {
        c.button("one", Box{10.0f, 10.0f, 80.0f, 24.0f});
        c.button("two", Box{10.0f, 40.0f, 80.0f, 24.0f});
        c.button("three", Box{10.0f, 70.0f, 80.0f, 24.0f});
    };

    // Nothing is focused until the keyboard asks for it -- a mouse user never sees a ring.
    h.frame(three);
    ASSERT_EQ(h.ctx.focus_id(), coopa::ui::imm::Id(0));
    ASSERT_TRUE(!h.ctx.focus_visible());

    h.press_key(coopa::input::Key::Tab);
    h.frame(three);
    const coopa::ui::imm::Id first = h.ctx.focus_id();
    ASSERT_TRUE(first != 0);
    ASSERT_TRUE(h.ctx.focus_visible());

    h.press_key(coopa::input::Key::Tab);
    h.frame(three);
    const coopa::ui::imm::Id second = h.ctx.focus_id();
    ASSERT_TRUE(second != first);

    h.press_key(coopa::input::Key::Tab);
    h.frame(three);
    const coopa::ui::imm::Id third = h.ctx.focus_id();
    ASSERT_TRUE(third != second && third != first);

    // Wraps rather than sticking at the end.
    h.press_key(coopa::input::Key::Tab);
    h.frame(three);
    ASSERT_EQ(h.ctx.focus_id(), first);

    // And back the other way, through the same cycle.
    h.press_key(coopa::input::Key::Tab, coopa::input::Mods::Shift);
    h.frame(three);
    ASSERT_EQ(h.ctx.focus_id(), third);
}

/** @brief Enter and Space activate the focused widget, so a form needs no mouse. */
COOPA_TEST(enter_and_space_activate_the_focused_widget) {
    Harness h;
    // Opt-in: a Context with a keymap of its own (the editor) leaves this off.
    h.ctx.keyboard_focus = true;
    int one_hits = 0, two_hits = 0;
    auto two = [&](Context& c) {
        if (c.button("one", Box{10.0f, 10.0f, 80.0f, 24.0f})) ++one_hits;
        if (c.button("two", Box{10.0f, 40.0f, 80.0f, 24.0f})) ++two_hits;
    };

    h.press_key(coopa::input::Key::Tab);
    h.frame(two);                       // focus lands on the first
    ASSERT_EQ(one_hits, 0);

    h.press_key(coopa::input::Key::Enter);
    h.frame(two);
    ASSERT_EQ(one_hits, 1);
    ASSERT_EQ(two_hits, 0);             // only the FOCUSED one fires

    h.press_key(coopa::input::Key::Tab);
    h.frame(two);
    h.press_key(coopa::input::Key::Space);
    h.frame(two);
    ASSERT_EQ(two_hits, 1);
    ASSERT_EQ(one_hits, 1);
}

/** @brief Clicking moves focus, so Tab continues from wherever the pointer left off. */
COOPA_TEST(clicking_a_widget_moves_focus_to_it) {
    Harness h;
    // Opt-in: a Context with a keymap of its own (the editor) leaves this off.
    h.ctx.keyboard_focus = true;
    auto two = [](Context& c) {
        c.button("one", Box{10.0f, 10.0f, 80.0f, 24.0f});
        c.button("two", Box{10.0f, 40.0f, 80.0f, 24.0f});
    };
    h.click({50.0f, 52.0f}, two);       // the second button
    const coopa::ui::imm::Id clicked = h.ctx.focus_id();
    ASSERT_TRUE(clicked != 0);

    // Tab from there lands on the FIRST, i.e. it continued from the clicked one and wrapped.
    h.press_key(coopa::input::Key::Tab);
    h.frame(two);
    ASSERT_TRUE(h.ctx.focus_id() != clicked);
}

COOPA_TEST(destroying_a_screens_object_takes_its_canvas_and_silences_the_callback) {
    struct Probe : toy::ui::ImmScreen {
        std::string type_name() const override { return "Dying"; }
        void draw(Context& ctx) override { ctx.fill(ctx.stretch(), glm::vec4(1.0f)); }
    };

    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Hud");
    Probe* probe = obj->add_component<Probe>();
    coopa::scene::SceneObject* raw = scene.add_root_object(std::move(obj));
    scene.adopt(*raw);
    scene.start();
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    ASSERT_TRUE(probe->canvas() != nullptr);

    // The canvas hangs off the screen's own object rather than off the scene root, which is
    // what makes its lifetime automatic.
    coopa::scene::SceneObject* canvas_obj = probe->canvas()->owner->parent();
    ASSERT_TRUE(canvas_obj != nullptr);
    ASSERT_TRUE(canvas_obj->parent() == raw);
    ASSERT_EQ(coopa::ui::collect_canvases(scene).size(), size_t(1));

    // Keep a copy of the callback alive past the screen, the way a canvas the host still holds
    // would. Invoking it after the owner is gone must do nothing at all.
    std::function<void(Context&)> stale = probe->canvas()->on_draw;
    ASSERT_TRUE(static_cast<bool>(stale));

    scene.remove_root_object(raw);
    ASSERT_EQ(coopa::ui::collect_canvases(scene).size(), size_t(0));

    Harness h;
    h.frame([&](Context& c) { stale(c); });   // the real check: no call into the dead screen
    ASSERT_EQ(h.dl.vertices().size(), size_t(0));

    // And the scene keeps running with neither the screen nor its canvas in it.
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);
    ASSERT_EQ(coopa::ui::collect_canvases(scene).size(), size_t(0));
}

COOPA_TEST(an_imm_screen_builds_its_canvas_and_draws_through_the_scene) {
    // The whole contract a game relies on: add the component, get a canvas, and draw() runs
    // once per frame with this frame's state.
    struct Probe : toy::ui::ImmScreen {
        std::string type_name() const override { return "Probe"; }
        int frames = 0;
        glm::vec2 seen{0.0f};
        void draw(Context& ctx) override {
            ++frames;
            seen = ctx.canvas_size();
            ctx.fill(ctx.stretch(), glm::vec4(1.0f));
        }
    };

    coopa::scene::Scene scene("game");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Hud");
    Probe* probe = obj->add_component<Probe>();
    coopa::scene::SceneObject* raw = scene.add_root_object(std::move(obj));
    scene.adopt(*raw);
    scene.start();

    // No canvas yet, deliberately: building one adds a scene object, and start() is walking
    // the very vector that would reallocate. update() queues the build and late_update()'s
    // flush_commands() applies it, so a canvas exists after one WHOLE frame, not half of one.
    ASSERT_TRUE(probe->canvas() == nullptr);
    scene.update(1.0f / 60.0f);
    ASSERT_TRUE(probe->canvas() == nullptr);
    scene.late_update(1.0f / 60.0f);
    ASSERT_TRUE(probe->canvas() != nullptr);
    ASSERT_TRUE(probe->context() != nullptr);

    coopa::ui::CanvasComponent* canvas =
        probe->canvas()->owner->parent()->get_component<coopa::ui::CanvasComponent>();
    ASSERT_TRUE(canvas != nullptr);
    canvas->set_viewport(1280, 720);

    canvas->preview_refresh();
    ASSERT_EQ(probe->frames, 1);
    ASSERT_VEC_NEAR(probe->seen, glm::vec2(1280.0f, 720.0f), 1e-4f);
    ASSERT_TRUE(canvas->draw_list().vertices().size() > 0);

    // `hidden` stops the declaration without tearing anything down, so toggling it costs a
    // frame rather than a rebuild.
    probe->hidden = true;
    canvas->preview_refresh();
    ASSERT_EQ(probe->frames, 1);
    ASSERT_EQ(canvas->draw_list().vertices().size(), size_t(0));

    probe->hidden = false;
    canvas->preview_refresh();
    ASSERT_EQ(probe->frames, 2);
}

COOPA_TEST(an_imm_screen_can_live_in_the_world_and_gets_a_tunables_set) {
    struct Plate : toy::ui::ImmScreen {
        std::string type_name() const override { return "Plate"; }
        void draw(Context& ctx) override {
            // Asking through tune() is what makes a panel appear in the designer.
            const auto b = tune().box(ctx, "bar", coopa::ui::imm::Anchor::BottomCenter,
                                      {0.0f, -4.0f}, {180.0f, 10.0f});
            ctx.value_bar(b, 0.5f);
        }
    };

    coopa::scene::Scene scene("world");
    auto obj = std::make_unique<coopa::scene::SceneObject>("Creature");
    obj->add_component<coopa::scene::TransformComponent>();
    Plate* plate = obj->add_component<Plate>();
    plate->space = toy::ui::ImmSpace::World;
    plate->world_size = glm::vec2(200.0f, 48.0f);
    coopa::scene::SceneObject* raw = scene.add_root_object(std::move(obj));
    scene.adopt(*raw);
    scene.start();
    // Queued by update(), applied by late_update()'s flush_commands().
    scene.update(1.0f / 60.0f);
    scene.late_update(1.0f / 60.0f);

    ASSERT_TRUE(plate->canvas() != nullptr);
    coopa::ui::CanvasComponent* canvas =
        plate->canvas()->owner->parent()->get_component<coopa::ui::CanvasComponent>();
    ASSERT_TRUE(canvas != nullptr);
    ASSERT_TRUE(canvas->is_world_space());
    ASSERT_VEC_NEAR(canvas->world_size, glm::vec2(200.0f, 48.0f), 1e-4f);

    canvas->preview_refresh();
    // The box it asked for is registered under the name it used, ready for a gizmo.
    ASSERT_EQ(plate->tune().declared().size(), size_t(1));
    ASSERT_EQ(plate->tune().declared()[0].name, std::string("bar"));
}
