/**
 * @file hub_test.cpp
 * @brief The project hub through real input: the ... menu, Open launches the project's editor, Rebuild
 * cleans and re-runs build.sh, Add asks for options only for a bare folder, new names are
 * snake_case, and Remove / Move to Trash.
 */

#include <coopa/testing/test.h>

#include <chrono>
#include <thread>

#include "editor/support/editor_session.h"
#include "hub/hub_app.h"

COOPA_TEST_SUITE("hub");

namespace toy::editor::testing {

namespace {

/** @brief A listed hub project whose "editor" is a script that records being opened. */
fs::path make_hub_project(const fs::path& dir) {
    write_text(dir / "game.toy", "format: toyproject\ntarget: game\nengine:\n  source: x\n  ref: abc\n");
    write_text(dir / "editor.sh", "#!/bin/sh\ntouch \"$(dirname \"$0\")/opened\"\n");
    fs::permissions(dir / "editor.sh", fs::perms::owner_all);
    write_text(dir / "build" / "game_editor", "");   // "built"
    toy::hub::list_project(dir);
    return dir;
}

} // namespace

/** @brief The hub's project actions, through real input: menu, Open, Rebuild, Add, Remove. */
COOPA_TEST(project_actions_through_real_input) {
    namespace hub = toy::hub;
    setenv("NO_INPUT", "1", 1);
    use_scratch_home();   // the hub's ~/.toyengine, and ~/.Trash for Move to Trash
    const fs::path base = coopa::test::scratch_dir("hub_ui");
    const fs::path a = make_hub_project(base / "Alpha");
    toy::core::Engine engine(hub::HubApp::engine_config(false), hub::HubApp::engine_options());
    hub::HubApp app(engine);
    InputDriver in{engine, std::max(1.0f, engine.display_scale())};
    tick(engine, 3);
    expect(app.projects().size() == 1, "the listed project shows");
    auto centre = [](const coopa::ui::imm::Box& b) { return glm::vec2(b.x + b.w * 0.5f, b.y + b.h * 0.5f); };

    // "..." opens the project menu (the row's own click target must not swallow it).
    in.click(centre(app.row_menu_box(0)));
    tick(engine, 2);
    expect(app.menu_shown_for() == 0, "the ... button opens the project menu");
    dump(engine, "hub_menu");
    in.key(coopa::input::Key::Escape);
    in.click({600, 600});
    tick(engine, 2);

    // Open launches the project's editor (here: a script that leaves a marker).
    in.click(centre(app.row_open_box(0)));
    for (int i = 0; i < 200 && !fs::exists(a / "opened"); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    expect(fs::exists(a / "opened"), "Open launches the project's editor");

    // Rebuild (clean): stale build output goes, then build.sh runs (here: leaves a marker).
    write_text(a / "build.sh", "#!/bin/sh\ntouch \"$(dirname \"$0\")/rebuilt\"\n");
    fs::permissions(a / "build.sh", fs::perms::owner_all);
    write_text(a / "build" / "stale", "");
    app.rebuild_project(app.projects()[0]);
    for (int i = 0; i < 500 && app.task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    tick(engine, 2);
    expect(!fs::exists(a / "build" / "stale") && fs::exists(a / "rebuilt"), "Rebuild clears build/ and re-runs build.sh");
    expect(app.status().find("done") != std::string::npos, "...and reports success");
    write_text(a / "build" / "game_editor", "");   // "built" again for the steps below

    // Add: a bare folder gets the options dialog; an existing project is listed straight away.
    const fs::path bare = base / "Bare";
    fs::create_directories(bare);
    app.add_folder(bare);
    tick(engine, 3);
    expect(app.modal_shown() == "Add Project", "adding a folder with no project asks for its options");
    dump(engine, "hub_add_options");
    in.key(coopa::input::Key::Escape);
    tick(engine, 2);
    expect(app.modal_shown().empty(), "Escape cancels the dialog");
    const fs::path existing = base / "Existing";
    write_text(existing / "existing.toy", "target: existing\nengine:\n  source: x\n  ref: abc\n");
    app.add_folder(existing);
    for (int i = 0; i < 500 && (app.task().running() || app.projects().size() < 2); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    expect(app.projects().size() == 2, "adding an existing project lists it (" + std::to_string(app.projects().size()) + ")");
    expect(app.modal_shown().empty(), "...without the options dialog (shown: " + app.modal_shown() + ")");

    // New project names are snake_case: the default, and whatever is typed.
    expect(app.project_form().name == "my_game", "the New project dialog suggests my_game");
    app.project_form().adding = false;
    app.project_form().location = base.string();
    app.project_form().name = "My Game";
    expect(app.project_form().target() == base / "my_game", "a typed \"My Game\" creates the folder my_game");

    // Remove: the confirmation, then Remove from List (files stay) / Move to Trash (folder goes).
    app.open_remove_dialog(app.projects()[0]);
    tick(engine, 3);
    expect(app.modal_shown() == "Remove Project", "Remove Project asks first");
    dump(engine, "hub_remove");
    in.key(coopa::input::Key::Escape);
    tick(engine, 2);
    hub::HubProject alpha;
    for (const auto& p : app.projects()) if (p.name == "Alpha") alpha = p;
    app.delete_project(alpha);
    for (int i = 0; i < 500 && app.task().running(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); tick(engine, 1); }
    tick(engine, 2);
    const char* home = std::getenv("HOME");
    expect(!fs::exists(a) && fs::exists(fs::path(home) / ".Trash" / "Alpha"), "Move to Trash moves the project folder to the Trash");
    expect(app.projects().size() == 1 && app.projects()[0].name == "Existing", "...and drops it from the list");
}

} // namespace toy::editor::testing
