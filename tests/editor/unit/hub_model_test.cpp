/**
 * @file hub_model_test.cpp
 * @brief The hub's model, device-free: ~/.toyengine/projects.yaml holds each listed project once (never
 * the engine checkout, never the editor's recents), project metadata reads from the .toy, the
 * hub's settings are its own, and tasks stream output and exit status.
 */

#include <coopa/testing/test.h>

#include "editor/support/fixtures.h"
#include "hub/hub_app.h"

COOPA_TEST_SUITE("hub_model");

namespace toy::editor::testing {

COOPA_TEST(project_list_settings_and_tasks) {
    namespace hub = toy::hub;
    const fs::path home = coopa::test::scratch_dir("hub_home");
    setenv("HOME", home.c_str(), 1);
    expect(hub::projects_file() == home / ".toyengine" / "projects.yaml" &&
           hub::settings_file() == home / ".toyengine" / "settings.yaml", "the hub keeps everything in ~/.toyengine");
    const fs::path a = coopa::test::scratch_dir("Alpha"), b = coopa::test::scratch_dir("Beta");
    write_text(a / "alpha.toy", "target: alpha\nengine:\n  source: x\n  ref: 0123456789abcdef0123\n");
    write_text(b / "beta.toy", "target: beta\nengine:\n  source: x\n  ref: main\n  link: /some/engine\n");
    hub::list_project(a);
    hub::list_project(b);
    hub::list_project(a.string() + "/");   // the same project, spelled differently
    expect(hub::read_project_list().size() == 2, "the list holds each project once");
    expect(!hub::list_project(ROOT_DIR) && hub::read_project_list().size() == 2, "the engine checkout is never listed");

    // Nothing is auto-detected: the editor's recent projects (which include the engine
    // checkout) never show up in the hub.
    toy::editor::Project::remember(ROOT_DIR);
    toy::editor::Project::remember(coopa::test::scratch_dir("only_in_editor_recents"));
    auto list = hub::load_projects();
    expect(list.size() == 2, "only listed projects are shown");
    auto find = [&](const std::string& n) -> const hub::HubProject* {
        for (const auto& p : list) if (p.name == n) return &p;
        return nullptr;
    };
    const auto* pa = find("Alpha");
    const auto* pb = find("Beta");
    expect(pa && pb, "projects are named after their folders");
    expect(pa && pa->target == "alpha" && !pa->linked() && pa->engine_label() == "0123456789", "a pinned project shows its short ref");
    expect(pb && pb->linked() && pb->engine_link == "/some/engine" && pb->engine_label() == "linked", "a linked project shows as linked");
    expect(pa && !pa->built() && pa->editor_binary() == a / "build" / "alpha_editor", "unbuilt projects report no editor binary");
    hub::forget_project(a);
    list = hub::load_projects();
    expect(list.size() == 1 && list[0].name == "Beta", "forget removes a project from the list (not from disk)");
    expect(fs::exists(a / "alpha.toy"), "...and leaves its files alone");

    // Settings: the hub's own, blender_dark until set.
    expect(std::string(hub::default_hub_theme()) == "blender_dark" && !hub::load_settings().contains("theme"),
           "the hub starts on blender_dark");
    toy::editor::Node st = hub::load_settings();
    st["theme"] = toy::editor::Node(std::string("unity_dark"));
    hub::save_settings(st);
    expect(get_string(hub::load_settings(), "theme") == "unity_dark", "the hub's theme persists in ~/.toyengine/settings.yaml");
    expect(!toy::editor::Project::load_prefs().contains("theme"), "...without touching the editor's preferences");

    // Tasks: combined output line by line, exit status, quoting.
    hub::Task t;
    t.start("t", "echo " + hub::shell_quote("it's") + "; echo two >&2; exit 3");
    t.wait();
    const auto lines = t.lines();
    expect(t.exit_code() == 3 && !t.succeeded(), "a task reports its exit status");
    expect(lines.size() == 3 && lines[1] == "it's" && lines[2] == "two", "stdout and stderr stream in, quoting intact");
    expect(hub::toyhub_command({"new", "a b"}).find("'a b'") != std::string::npos, "toyhub arguments are shell-quoted");
}

} // namespace toy::editor::testing
