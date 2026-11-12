/**
 * @file editor_theme_test.cpp
 * @brief The editor's bundled UI themes (editor/themes/): every one loads, and Blender Dark -- the
 *        default, and the reference the others inherit from -- spells out every themable role, so
 *        no role silently falls back to a built-in colour.
 *
 * Not here: switching themes in a running editor (gpu settings).
 */

#include <coopa/testing/test.h>

#include <iostream>
#include <string>
#include <vector>

#include <root_directory.h>   // editor_theme.h names ROOT_DIR without including it

#include "editor/app/editor_theme.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("editor_theme");

namespace toy::editor::testing {

COOPA_TEST(bundled_themes_load_and_the_default_lists_every_role) {
    namespace imm = coopa::ui::imm;
    const auto themes = imm::list_themes(editor_themes_dir());
    expect(themes.size() >= 3, "editor/themes ships several themes (" + std::to_string(themes.size()) + ")");
    for (const auto& e : themes) {
        bool ok = true;
        try { imm::load_theme(e.path); } catch (const std::exception& ex) { ok = false; std::cerr << ex.what() << "\n"; }
        expect(ok, "theme '" + e.id + "' loads");
    }
    // The default theme is the complete reference: every Style field and editor role present.
    const fkyaml::node dark = coopa::yaml::load_document(editor_themes_dir() / "blender_dark.yaml");
    std::vector<std::string> missing;
    struct Presence {
        const fkyaml::node* root; std::vector<std::string>* missing;
        void metric(const char* k, const float&) const { if (!root->at("metrics").contains(k)) missing->push_back(std::string("metrics.") + k); }
        void color(const char* k, const glm::vec4&) const { if (!root->at("colors").contains(k)) missing->push_back(std::string("colors.") + k); }
    };
    const imm::Style ref_style;
    const EditorTheme ref_editor;
    imm::visit_style(ref_style, Presence{&dark, &missing});
    visit_editor_theme(ref_editor, [&](const char* sec, const char* role, const glm::vec4&) {
        if (!dark.contains(sec) || !dark.at(sec).contains(role)) missing.push_back(std::string(sec) + "." + role);
    });
    std::string list;
    for (const auto& m : missing) list += " " + m;
    expect(missing.empty(), "blender_dark.yaml lists every themable role (missing:" + list + ")");
}

} // namespace toy::editor::testing
