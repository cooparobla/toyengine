/**
 * @file glass_theme_test.cpp
 * @brief A glassmorphism screen's own theme section: roles load, omitted ones keep their
 *        defaults, `inherits:` chains, and every role in the visit list is reachable.
 *
 * Headless -- a theme is YAML and colour arithmetic, no device involved. Mirrors
 * game_theme_test.cpp's shape exactly, since GlassTheme is modelled field-for-field on
 * GameTheme.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <string>

#include <toyengine/ui/glass_theme.h>
#include <uicoopa/immediate/imm_theme.h>

COOPA_TEST_SUITE("glass_theme");

namespace {

std::filesystem::path write_theme(const std::string& name, const std::string& text) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "coopa_tests" / "glass_theme";
    std::filesystem::create_directories(dir);
    const std::filesystem::path p = dir / (name + ".yaml");
    std::ofstream(p) << text;
    return p;
}

}  // namespace

COOPA_TEST(roles_load_from_the_glass_section_and_omitted_ones_keep_defaults) {
    const auto p = write_theme("partial", R"(name: Partial
glass:
  fill: "#ffffff40"
  grad_blue: [0.0, 0.0, 1.0, 1.0]
)");
    const coopa::ui::imm::Theme theme = coopa::ui::imm::load_theme(p);
    const toy::ui::GlassTheme t = toy::ui::glass_theme_from(theme);
    const toy::ui::GlassTheme defaults;

    ASSERT_VEC_NEAR(t.fill, glm::vec4(1.0f, 1.0f, 1.0f, 64.0f / 255.0f), 1e-3f);
    ASSERT_VEC_NEAR(t.grad_blue, glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), 1e-3f);

    // Everything else keeps its default -- a theme can state two colours and no more.
    ASSERT_VEC_NEAR(t.grad_violet, defaults.grad_violet, 1e-6f);
    ASSERT_VEC_NEAR(t.blob_teal, defaults.blob_teal, 1e-6f);
    ASSERT_VEC_NEAR(t.opaque_fill, defaults.opaque_fill, 1e-6f);
}

COOPA_TEST(a_glass_theme_inherits_its_base) {
    write_theme("gl_base", R"(name: GL Base
glass:
  grad_blue: "#101010"
  grad_pink: "#aa00aa"
)");
    const auto p = write_theme("gl_child", R"(name: GL Child
inherits: gl_base
glass:
  grad_pink: "#00ff00"
)");
    const toy::ui::GlassTheme t = toy::ui::glass_theme_from(coopa::ui::imm::load_theme(p));
    // Overridden in the child...
    ASSERT_VEC_NEAR(t.grad_pink, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 1e-3f);
    // ...inherited from the base.
    ASSERT_VEC_NEAR(t.grad_blue, glm::vec4(16.0f / 255.0f, 16.0f / 255.0f, 16.0f / 255.0f, 1.0f), 5e-3f);
}

COOPA_TEST(every_role_in_the_visit_list_is_actually_readable) {
    // visit_glass_theme() is the single list the loader walks, so a role added to the struct
    // but forgotten there would silently never load. Prove each one round-trips.
    std::string yaml = "name: All\nglass:\n";
    int n = 0;
    toy::ui::GlassTheme probe;
    toy::ui::visit_glass_theme(probe, [&](const char*, const char* role, glm::vec4&) {
        const float r = static_cast<float>(++n) / 255.0f;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  %s: [%.6f, 0.0, 0.0, 1.0]\n", role, r);
        yaml += buf;
    });
    ASSERT_TRUE(n >= 9);   // the list is populated at all

    const toy::ui::GlassTheme t = toy::ui::glass_theme_from(
        coopa::ui::imm::load_theme(write_theme("all_roles", yaml)));
    int k = 0;
    toy::ui::GlassTheme check = t;
    toy::ui::visit_glass_theme(check, [&](const char*, const char* role, glm::vec4& c) {
        const float want = static_cast<float>(++k) / 255.0f;
        if (std::abs(c.r - want) > 1e-4f) {
            std::printf("role '%s' did not load: got %.6f want %.6f\n", role, c.r, want);
        }
        ASSERT_NEAR(c.r, want, 1e-4f);
    });
}
