/**
 * @file ui_panel_test.cpp
 * @brief Screens authored as YAML: parsing, the binding indirection, actions, and the
 *        shipped panel assets.
 *
 * The point of the format is that a screen needs no C++, so these tests use no screen class --
 * they parse a panel and drive draw_panel() directly.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <toyengine/core/user_settings.h>
#include <toyengine/ui/ui_panel.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/render/draw_list.h>

COOPA_TEST_SUITE("ui_panel");

namespace {

using coopa::ui::imm::Box;
using coopa::ui::imm::Context;

/// One imm frame over a scratch DrawList, with a scripted pointer.
struct Frame {
    Context ctx;
    coopa::ui::DrawList dl;
    glm::vec2 size{900.0f, 700.0f};
    glm::vec2 mouse{-1.0e6f};
    bool down = false, prev = false;

    void run(const std::function<void(Context&)>& fn) {
        coopa::ui::imm::FrameInput in;
        in.mouse = mouse;
        in.down[0] = down;
        in.pressed[0] = down && !prev;
        in.released[0] = !down && prev;
        prev = down;
        dl.begin(coopa::ui::Rect{glm::vec2(0.0f), size});
        ctx.begin_frame(dl, in, size);
        fn(ctx);
        ctx.end_frame();
    }
    /// Press and release over `at`; imm resolves a click on release against last frame's layers.
    void click(glm::vec2 at, const std::function<void(Context&)>& fn) {
        mouse = at; run(fn);
        down = true;  run(fn);
        down = false; run(fn);
    }
};

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

COOPA_TEST(a_panel_parses_tabs_rows_and_buttons) {
    const toy::ui::UiPanel p = toy::ui::parse_panel(R"(
title: Settings
anchor: TopRight
size: [400, 300]
offset: [-20, 20]
closable: true
tabs:
  - name: Audio
    rows:
      - { kind: slider, label: Master, bind: audio.master, min: 0, max: 2, tooltip: Everything }
      - { kind: toggle, label: Subtitles, bind: ui.subs }
      - { kind: separator }
      - { kind: combo, label: Quality, bind: render.quality, options: [Low, High] }
  - name: Controls
    rows:
      - { kind: int, label: Slots, bind: ui.slots, min: 1, max: 10 }
buttons:
  - { label: Apply, action: apply }
  - { label: Close }
)");

    ASSERT_EQ(p.title, std::string("Settings"));
    ASSERT_TRUE(p.anchor == coopa::ui::imm::Anchor::TopRight);
    ASSERT_TRUE(p.closable);
    ASSERT_VEC_NEAR(p.size, glm::vec2(400.0f, 300.0f), 1e-4f);
    ASSERT_VEC_NEAR(p.offset, glm::vec2(-20.0f, 20.0f), 1e-4f);

    ASSERT_EQ(p.tabs.size(), size_t(2));
    ASSERT_EQ(p.tabs[0].name, std::string("Audio"));
    ASSERT_EQ(p.tabs[0].rows.size(), size_t(4));
    ASSERT_TRUE(p.tabs[0].rows[0].kind == toy::ui::RowKind::Slider);
    ASSERT_NEAR(p.tabs[0].rows[0].max, 2.0f, 1e-4f);
    ASSERT_EQ(p.tabs[0].rows[0].tooltip, std::string("Everything"));
    ASSERT_TRUE(p.tabs[0].rows[2].kind == toy::ui::RowKind::Separator);
    ASSERT_EQ(p.tabs[0].rows[3].options.size(), size_t(2));
    ASSERT_TRUE(p.tabs[1].rows[0].kind == toy::ui::RowKind::Int);

    ASSERT_EQ(p.buttons.size(), size_t(2));
    ASSERT_EQ(p.buttons[0].action, std::string("apply"));
    // A button with no action still reports something, so an authored panel is never silent.
    ASSERT_EQ(p.buttons[1].action, std::string("Close"));
}

COOPA_TEST(an_untabbed_panel_becomes_one_group) {
    // Drawing has a single shape to handle, so `rows:` at the top level is folded into one
    // unnamed tab rather than being a second code path.
    const toy::ui::UiPanel p = toy::ui::parse_panel(R"(
title: Pause
rows:
  - { kind: button, label: Resume, action: resume }
  - { kind: button, label: Quit,   action: quit }
)");
    ASSERT_EQ(p.tabs.size(), size_t(1));
    ASSERT_EQ(p.tabs[0].rows.size(), size_t(2));
    ASSERT_TRUE(p.tabs[0].name.empty());
}

COOPA_TEST(malformed_yaml_throws_rather_than_drawing_nothing) {
    bool threw = false;
    try {
        toy::ui::parse_panel("- this is a sequence, not a panel\n");
    } catch (const std::exception&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

COOPA_TEST(a_row_reads_and_writes_user_settings_with_no_game_code) {
    // The whole reason an authored panel is useful before any C++ exists.
    const std::filesystem::path file =
        coopa::test::scratch_dir("ui_panel") / "settings.yaml";
    toy::core::UserSettings::instance().load(file);
    toy::core::UserSettings::instance().set_float("audio.master", 0.4f);

    toy::ui::PanelBindings b;
    ASSERT_NEAR(b.get("audio.master", -1.0f), 0.4f, 1e-4f);
    b.set("audio.master", 0.9f);
    ASSERT_NEAR(toy::core::UserSettings::instance().get_float("audio.master", -1.0f), 0.9f, 1e-4f);

    // An unset key falls through to the caller's default rather than to zero -- a slider
    // that started at 0 would mute a game the first time its screen opened.
    ASSERT_NEAR(b.get("never.set.by.anyone", 0.75f), 0.75f, 1e-4f);

    toy::core::UserSettings::instance().load(coopa::test::scratch_dir() / "none.yaml");
}

COOPA_TEST(a_provider_overrides_user_settings_for_its_key) {
    float fov = 70.0f;
    toy::ui::PanelBindings b;
    b.provide("player.fov", [&] { return fov; }, [&](float v) { fov = v; });

    ASSERT_NEAR(b.get("player.fov", 0.0f), 70.0f, 1e-4f);
    b.set("player.fov", 95.0f);
    ASSERT_NEAR(fov, 95.0f, 1e-4f);
    // And it did NOT leak into UserSettings: a game value is the game's to persist.
    ASSERT_NEAR(toy::core::UserSettings::instance().get_float("player.fov", -1.0f), -1.0f, 1e-4f);
}

COOPA_TEST(a_button_reports_its_action_and_runs_its_handler) {
    const toy::ui::UiPanel p = toy::ui::parse_panel(R"(
rows:
  - { kind: button, label: Resume, action: resume }
)");
    toy::ui::PanelBindings b;
    int ran = 0;
    b.on_action("resume", [&] { ++ran; });

    Frame f;
    std::string fired;
    int tab = 0;
    // A row button is sized to its LABEL, not to the panel: it sits at the body's left edge.
    const Box box{0.0f, 0.0f, 400.0f, 300.0f};
    f.click({24.0f, 10.0f}, [&](Context& c) {
        const std::string a = toy::ui::draw_panel(c, box, p, b, &tab);
        if (!a.empty()) fired = a;
    });
    ASSERT_EQ(fired, std::string("resume"));
    ASSERT_EQ(ran, 1);
}

COOPA_TEST(every_shipped_bind_key_has_something_that_consumes_it) {
    // The check that has teeth. Asserting a row merely HAS a `bind` passes for `ui.scale`
    // when the engine reads `ui.hud_scale` -- the row then persists a key nothing will ever
    // read, and looks identical to one that works. unbound_keys() compares against the keys
    // the engine actually consumes, so a typo fails here instead of shipping.
    const std::filesystem::path ui_dir = std::filesystem::path(PROJ_DIR) / ".." / "assets" / "ui";
    for (const char* name : {"settings.yaml", "pause_menu.yaml"}) {
        const toy::ui::UiPanel p = toy::ui::parse_panel(read_file(ui_dir / name));
        toy::ui::PanelBindings none;
        const std::vector<std::string> dead = toy::ui::unbound_keys(p, none);
        std::string joined;
        for (const std::string& k : dead) joined += (joined.empty() ? "" : ", ") + k;
        coopa::test::expect(dead.empty(),
                            std::string(name) + ": every bound key has a consumer (dead: " +
                                (joined.empty() ? "none" : joined) + ")");
    }

    // And a provider counts as a consumer, which is how a game adds its own rows.
    toy::ui::UiPanel custom;
    custom.tabs.push_back({"Game", {toy::ui::UiRow{}}});
    custom.tabs[0].rows[0].kind = toy::ui::RowKind::Toggle;
    custom.tabs[0].rows[0].bind = "game.subtitles";
    toy::ui::PanelBindings b;
    ASSERT_EQ(toy::ui::unbound_keys(custom, b).size(), size_t(1));
    bool on = false;
    b.provide_bool("game.subtitles", [&] { return on; }, [&](bool v) { on = v; });
    ASSERT_TRUE(toy::ui::unbound_keys(custom, b).empty());
}

COOPA_TEST(the_shipped_panels_parse_and_are_well_formed) {
    // A typo in an authored panel is a dead row, which looks like a layout bug. Parse the
    // ones that ship and check every bound row actually names a key.
    const std::filesystem::path assets = std::filesystem::path(PROJ_DIR) / ".." / "assets" / "ui";
    for (const char* name : {"settings.yaml", "pause_menu.yaml"}) {
        const std::filesystem::path path = assets / name;
        ASSERT_TRUE(std::filesystem::exists(path));
        const toy::ui::UiPanel p = toy::ui::parse_panel(read_file(path));
        ASSERT_TRUE(!p.tabs.empty());
        for (const toy::ui::UiTab& t : p.tabs) {
            for (const toy::ui::UiRow& r : t.rows) {
                const bool needs_bind = r.kind == toy::ui::RowKind::Slider ||
                                        r.kind == toy::ui::RowKind::Toggle ||
                                        r.kind == toy::ui::RowKind::Int ||
                                        r.kind == toy::ui::RowKind::Combo;
                if (needs_bind) {
                    ASSERT_TRUE(!r.bind.empty());
                    ASSERT_TRUE(!r.label.empty());
                }
                if (r.kind == toy::ui::RowKind::Combo) ASSERT_TRUE(!r.options.empty());
                if (r.kind == toy::ui::RowKind::Slider) ASSERT_TRUE(r.max > r.min);
            }
        }
    }
}

COOPA_TEST(drawing_a_panel_is_finite_at_every_row_kind) {
    const toy::ui::UiPanel p = toy::ui::parse_panel(R"(
title: Everything
tabs:
  - name: A
    rows:
      - { kind: label, label: Just text }
      - { kind: slider, label: S, bind: a.s, min: 0, max: 1 }
      - { kind: toggle, label: T, bind: a.t }
      - { kind: int, label: I, bind: a.i, min: 0, max: 9 }
      - { kind: combo, label: C, bind: a.c, options: [One, Two] }
      - { kind: separator }
      - { kind: spacer }
      - { kind: button, label: B, action: b }
  - name: Empty
    rows: []
buttons:
  - { label: Close, action: close }
)");
    toy::ui::PanelBindings b;
    Frame f;
    int tab = 0;
    for (int t = 0; t < 2; ++t) {
        tab = t;
        f.run([&](Context& c) { toy::ui::draw_panel(c, {20.0f, 20.0f, 500.0f, 400.0f}, p, b, &tab); });
    }
    ASSERT_TRUE(f.dl.vertices().size() > 0);
    for (const coopa::ui::UiVertex& v : f.dl.vertices()) {
        ASSERT_TRUE(std::isfinite(v.x) && std::isfinite(v.y));
    }
}
