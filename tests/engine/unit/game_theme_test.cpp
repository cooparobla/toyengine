/**
 * @file game_theme_test.cpp
 * @brief A game's theme as a section of an immediate-mode theme: roles load, omitted ones keep
 *        their defaults, `inherits:` chains, and the bar/slot style mapping.
 *
 * Headless -- a theme is YAML and colour arithmetic, no device involved.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <string>

#include <toyengine/ui/game_theme.h>
#include <uicoopa/immediate/imm_theme.h>

COOPA_TEST_SUITE("game_theme");

namespace {

/// Writes `text` to a scratch .yaml and returns its path.
std::filesystem::path write_theme(const std::string& name, const std::string& text) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "coopa_tests" / "game_theme";
    std::filesystem::create_directories(dir);
    const std::filesystem::path p = dir / (name + ".yaml");
    std::ofstream(p) << text;
    return p;
}

}  // namespace

COOPA_TEST(roles_load_from_the_game_section_and_omitted_ones_keep_defaults) {
    const auto p = write_theme("partial", R"(name: Partial
colors:
  accent: "#ffcc00"
bars:
  health: "#ff0000"
feedback:
  critical: [1.0, 0.5, 0.0, 1.0]
)");
    const coopa::ui::imm::Theme theme = coopa::ui::imm::load_theme(p);
    const toy::ui::GameTheme t = toy::ui::game_theme_from(theme);
    const toy::ui::GameTheme defaults;

    // Stated roles win, in either colour spelling.
    ASSERT_VEC_NEAR(t.bars.health, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), 1e-3f);
    ASSERT_VEC_NEAR(t.feedback.critical, glm::vec4(1.0f, 0.5f, 0.0f, 1.0f), 1e-3f);

    // Everything else keeps its default, so a project can state two colours and no more --
    // the whole reason a role has a built-in value.
    ASSERT_VEC_NEAR(t.bars.mana, defaults.bars.mana, 1e-6f);
    ASSERT_VEC_NEAR(t.slots.empty, defaults.slots.empty, 1e-6f);
    ASSERT_VEC_NEAR(t.world.hostile, defaults.world.hostile, 1e-6f);

    // And the widget Style came across from the same file, so one theme styles both the
    // panels a screen sits on and the roles it paints with.
    ASSERT_VEC_NEAR(theme.style.accent, glm::vec4(1.0f, 0.8f, 0.0f, 1.0f), 4e-3f);
}

COOPA_TEST(a_game_theme_inherits_its_base) {
    write_theme("gt_base", R"(name: GT Base
colors:
  panel_bg: "#101010"
bars:
  health: "#aa0000"
  mana: "#0000aa"
)");
    const auto p = write_theme("gt_child", R"(name: GT Child
inherits: gt_base
bars:
  health: "#00ff00"
)");
    const toy::ui::GameTheme t = toy::ui::game_theme_from(coopa::ui::imm::load_theme(p));
    // Overridden in the child...
    ASSERT_VEC_NEAR(t.bars.health, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 1e-3f);
    // ...inherited from the base, which is what makes a variant a handful of lines.
    ASSERT_VEC_NEAR(t.bars.mana, glm::vec4(0.0f, 0.0f, 2.0f / 3.0f, 1.0f), 5e-3f);
}

COOPA_TEST(every_role_in_the_visit_list_is_actually_readable) {
    // visit_game_theme() is the single list the loader walks, so a role added to the struct
    // but forgotten there would silently never load. Prove each one round-trips by writing a
    // file that sets them all to a unique colour.
    std::string yaml = "name: All\n";
    std::string section;
    int n = 0;
    toy::ui::GameTheme probe;
    toy::ui::visit_game_theme(probe, [&](const char* sec, const char* role, glm::vec4&) {
        if (section != sec) {
            section = sec;
            yaml += std::string(sec) + ":\n";
        }
        // A distinct red channel per role, so a mis-mapped one shows up as the wrong value.
        const float r = static_cast<float>(++n) / 255.0f;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  %s: [%.6f, 0.0, 0.0, 1.0]\n", role, r);
        yaml += buf;
    });
    ASSERT_TRUE(n > 15);   // the list is populated at all

    const toy::ui::GameTheme t = toy::ui::game_theme_from(
        coopa::ui::imm::load_theme(write_theme("all_roles", yaml)));
    int k = 0;
    toy::ui::GameTheme check = t;
    toy::ui::visit_game_theme(check, [&](const char*, const char* role, glm::vec4& c) {
        const float want = static_cast<float>(++k) / 255.0f;
        if (std::abs(c.r - want) > 1e-4f) {
            std::printf("role '%s' did not load: got %.6f want %.6f\n", role, c.r, want);
        }
        ASSERT_NEAR(c.r, want, 1e-4f);
    });
}

COOPA_TEST(bar_and_slot_styles_map_roles_and_leave_the_rest_to_the_widget_style) {
    toy::ui::GameTheme t;
    t.bars.health = glm::vec4(0.9f, 0.1f, 0.1f, 1.0f);
    t.bars.damage_trail = glm::vec4(0.9f, 0.1f, 0.1f, 0.5f);
    t.slots.empty = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f);
    t.slots.hover = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);

    const coopa::ui::imm::BarStyle bs = toy::ui::bar_style(t, t.bars.health);
    ASSERT_VEC_NEAR(bs.fill, t.bars.health, 1e-6f);
    ASSERT_VEC_NEAR(bs.trail, t.bars.damage_trail, 1e-6f);
    // track and label stay UNSET (a negative red is the sentinel), so they resolve from the
    // widget Style -- a game's bars should sit against its panels, not carry their own idea
    // of what a background is.
    ASSERT_TRUE(bs.track.r < 0.0f);
    ASSERT_TRUE(bs.label.r < 0.0f);
    ASSERT_TRUE(bs.rounding < 0.0f);

    const coopa::ui::imm::SlotStyle ss = toy::ui::slot_style(t);
    ASSERT_VEC_NEAR(ss.cell, t.slots.empty, 1e-6f);
    ASSERT_VEC_NEAR(ss.cell_hover, t.slots.hover, 1e-6f);
    ASSERT_TRUE(ss.border.r < 0.0f);
    ASSERT_TRUE(ss.spacing < 0.0f);
}
