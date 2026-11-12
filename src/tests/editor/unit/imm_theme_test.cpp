/**
 * @file imm_theme_test.cpp
 * @brief imm theme files: every colour format, inheritance of unmentioned fields, app sections, errors
 * that name their key, and a lossless theme_to_yaml round trip.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <uicoopa/immediate/imm_theme.h>

#include "editor/support/imm_harness.h"

COOPA_TEST_SUITE("imm_theme");

namespace toy::editor::testing {

/** @brief imm themes: colour formats, inheritance, app sections, errors, and a lossless round trip. */
COOPA_TEST(parse_formats_errors_and_lossless_round_trip) {
    namespace imm = coopa::ui::imm;
    const imm::Theme t = imm::parse_theme(R"(
name: Test
metrics:
  font_size: 14
  rounding: 2.5
colors:
  accent: "#ff8000"
  selection: "#11223380"
  text: [0.5, 0.25, 1.0]
  panel_bg: {r: 0.1, g: 0.2, b: 0.3, a: 0.4}
viewport:
  grid: "#fff"
)");
    expect(t.name == "Test" && t.style.font_size == 14.0f && t.style.rounding == 2.5f, "metrics apply");
    expect(glm::distance(t.style.accent, glm::vec4(1.0f, 128 / 255.0f, 0.0f, 1.0f)) < 1e-4f, "#rrggbb colours");
    expect(std::abs(t.style.selection.a - 128 / 255.0f) < 1e-4f, "#rrggbbaa carries alpha");
    expect(t.style.text == glm::vec4(0.5f, 0.25f, 1.0f, 1.0f), "[r, g, b] float lists (alpha 1)");
    expect(t.style.panel_bg == glm::vec4(0.1f, 0.2f, 0.3f, 0.4f), "{r, g, b, a} maps");
    expect(t.color("viewport", "grid", glm::vec4(0)) == glm::vec4(1), "app sections, #rgb shorthand");
    expect(t.color("viewport", "missing", glm::vec4(0.5f)) == glm::vec4(0.5f), "unknown roles fall back");
    expect(t.style.row_height == imm::Style{}.row_height, "unmentioned fields keep the built-in value");

    bool threw = false;
    try { imm::parse_theme("colors:\n  accent: \"#12345\"\n"); } catch (const std::exception& e) {
        threw = std::string(e.what()).find("colors.accent") != std::string::npos;
    }
    expect(threw, "a malformed colour names its key");

    // Round trip: every field survives save -> load (to 8-bit colour precision).
    const imm::Theme back = imm::parse_theme(imm::theme_to_yaml(t));
    struct Flat {
        std::vector<float>* out;
        void metric(const char*, const float& v) const { out->push_back(v); }
        void color(const char*, const glm::vec4& c) const { for (int i = 0; i < 4; ++i) out->push_back(c[i]); }
    };
    std::vector<float> a, b;
    imm::visit_style(t.style, Flat{&a});
    imm::visit_style(back.style, Flat{&b});
    bool same = back.name == t.name && back.sections == t.sections && a.size() == b.size();
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) same = same && std::abs(a[i] - b[i]) < 0.003f;
    expect(same, "theme_to_yaml round-trips every field");
}

} // namespace toy::editor::testing
