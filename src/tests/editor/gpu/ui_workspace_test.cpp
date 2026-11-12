/**
 * @file ui_workspace_test.cpp
 * @brief The UI workspace: the two editors it opens, and that each round-trips its file.
 *
 * Game UI is immediate-mode, so there is no canvas to click and nothing to link -- what is left
 * to author is data, and the workspace is a form over it. These cover the two things that can
 * silently not work: which editor a file opens (the FILE decides, not a toggle), and whether an
 * edit made through the form actually reaches disk.
 *
 * The Placement editor is the one worth a device: it learns a screen's boxes by RUNNING the
 * screen component in a preview scene, which needs a real Engine, so there is no headless
 * shortcut.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <algorithm>

#include "editor/app/ui/ui_canvas.h"
#include "toyengine/ui/imm_screen.h"
#include "toyengine/ui/panel_screen.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("ui_workspace");

namespace toy::editor::testing {
namespace {

std::string read(const std::filesystem::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void write(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << text;
}

}  // namespace

/**
 * @brief A panel asset opens the Panel editor, and a row edited through it reaches disk.
 *
 * Also covers the row operations the list had no way to do at all: a list you can only append
 * to is not an editor, because a row's ORDER is its layout -- rows draw in file order.
 */
COOPA_TEST(a_panel_asset_opens_the_row_editor_and_saves_what_it_changed) {
    EditorSession s;
    const std::filesystem::path panel = s.project.assets() / "ui" / "probe.yaml";
    write(panel,
          "title: Probe\nanchor: Center\nsize: [400, 300]\n"
          "rows:\n"
          "  - { kind: label,  label: First }\n"
          "  - { kind: slider, label: Second, bind: audio.master, min: 0, max: 1 }\n"
          "  - { kind: button, label: Third,  action: go }\n");
    s.project.refresh();

    UiCanvasPanel& ui = s.app.ui_workspace();
    coopa::test::require(ui.open_ui_asset(panel), "the panel opened");
    coopa::test::expect(ui.mode() == UiCanvasPanel::Mode::Panel, "a .yaml panel opens the Panel editor");
    coopa::test::expect(!UiCanvasPanel::is_placement_file(panel), "and is not mistaken for a placement file");

    // Move the last row to the top, then save. Order is the thing being tested: rows draw in
    // the order the file states them.
    ui.select_row(2);
    ui.move_row_(-1);
    ui.move_row_(-1);
    coopa::test::expect(ui.dirty(), "moving a row is an edit");
    ui.save();

    const std::string saved = read(panel);
    const size_t third = saved.find("Third");
    const size_t first = saved.find("First");
    coopa::test::require(third != std::string::npos && first != std::string::npos, "both rows survived the save");
    coopa::test::expect(third < first, "the moved row is now before the one it passed");

    // Undo puts it back, which is what makes the operation safe to try.
    ui.undo();
    ui.save();
    const std::string back = read(panel);
    coopa::test::expect(back.find("First") < back.find("Third"), "undo restored the original order");
}

/**
 * @brief A `.tune.yaml` opens the Placement editor, learns the screen's boxes by running it,
 *        and a moved box reaches disk.
 *
 * The file is named after the screen (`demo_hud.tune.yaml` for `DemoHud`), which is the whole
 * binding -- there
 * is no "which screen is this for" field to get wrong. The boxes cannot come from the file,
 * because a file that has never been written states none: they come from the screen DECLARING
 * them as it draws, which is why this needs the component to actually run.
 */
COOPA_TEST(a_placement_file_opens_the_box_editor_and_learns_the_screens_boxes) {
    EditorSession s;
    const std::filesystem::path tune = s.project.assets() / "ui" / "demo_hud.tune.yaml";
    write(tune, "# empty: every box keeps the placement its own code states\n");
    s.project.refresh();

    UiCanvasPanel& ui = s.app.ui_workspace();
    coopa::test::expect(UiCanvasPanel::is_placement_file(tune), "the suffix marks it as a placement file");
    // Opened through the app, not the panel directly: the preview only RUNS while its
    // workspace is showing (its scene is an overlay layer, and the engine updates those only
    // while they are attached), so a test that skips the workspace never gets a drawn screen.
    s.app.open_asset(AssetType::UI, "ui/demo_hud.tune.yaml");
    // open_asset() routes through the editor's save guard, which may defer the open by a
    // frame, so the workspace is checked after ticking rather than on the same line.
    tick(s.engine, 2);
    coopa::test::require(s.app.active_asset_type() == AssetType::UI, "the UI workspace opened");
    coopa::test::expect(ui.mode() == UiCanvasPanel::Mode::Placement, "a .tune.yaml opens the Placement editor");

    // DemoHud is offered because it is a UI-category component schema; PanelScreen is not,
    // because its geometry belongs to the panel asset it draws.
    const std::vector<std::string> types = UiCanvasPanel::screen_types();
    coopa::test::expect(std::find(types.begin(), types.end(), "DemoHud") != types.end(),
           "DemoHud is an offerable screen");
    coopa::test::expect(std::find(types.begin(), types.end(), "PanelScreen") == types.end(),
           "PanelScreen declares no boxes, so it is not offered");

    // The screen has to DRAW before it has declared anything, so give the preview some frames.
    tick(s.engine, 8);
    toy::ui::ImmScreen* screen = ui.placed_screen();
    coopa::test::require(screen != nullptr, "the screen component is running in the preview");
    const std::vector<toy::ui::Tunable>& boxes = screen->tune().declared();
    coopa::test::require(!boxes.empty(), "the screen declared its boxes by drawing them (" +
                                std::to_string(boxes.size()) + ")");

    // Nothing is in the file yet, so each box reads its code default.
    const std::string name = boxes[0].name;
    const glm::vec2 before = boxes[0].pos;

    screen->tune().set(name, boxes[0].anchor, before + glm::vec2(40.0f, 24.0f), boxes[0].size);
    ui.mark_dirty();
    ui.save();

    const std::string saved = read(tune);
    coopa::test::expect(saved.find(name) != std::string::npos,
           "the moved box is named in the file (" + name + ")");

    // And it comes back: a fresh Tunables over the saved file states that box.
    toy::ui::Tunables reloaded;
    // Through the shared helper, which is the point of it: the screen derives the file it
    // looks for from its type name, so a test that spells the name by hand could pass while
    // the screen looked somewhere else.
    reloaded.load("ui/" + toy::ui::tune_file_stem("DemoHud"));
    const toy::ui::Tunable* e = reloaded.find(name);
    coopa::test::require(e != nullptr, "the saved entry reloads");
    coopa::test::expect_near(e->pos.x, before.x + 40.0f, 0.5f, "the x it was moved to survived the round trip");
    coopa::test::expect_near(e->pos.y, before.y + 24.0f, 0.5f, "and the y");
}

/**
 * @brief The preview stops drawing the moment the workspace is left.
 *
 * The preview is an engine OVERLAY LAYER, and the engine holds those non-owning and draws them
 * every frame until something removes them. So "opened a UI once" and "is looking at the UI
 * workspace" are different questions, and attaching on open while only detaching on teardown
 * leaves an authored HUD painted over the scene view for the rest of the session -- which is
 * exactly the thing the workspace exists to avoid.
 */
COOPA_TEST(the_preview_leaves_the_viewport_when_the_workspace_does) {
    EditorSession s;
    const std::filesystem::path panel = s.project.assets() / "ui" / "overlay_probe.yaml";
    write(panel,
          "title: Probe\nanchor: Center\nsize: [400, 300]\n"
          "rows:\n"
          "  - { kind: label, label: Only }\n");
    s.project.refresh();

    const size_t base = s.engine.overlay_layers().size();

    s.app.open_asset(AssetType::UI, "ui/overlay_probe.yaml");
    tick(s.engine, 4);

    coopa::test::expect(s.app.active_asset_type() == AssetType::UI, "the UI workspace is showing");
    const size_t with_ui = s.engine.overlay_layers().size();
    coopa::test::require(with_ui == base + 1,
           "the preview added exactly one overlay layer while the workspace is open (" +
               std::to_string(base) + " -> " + std::to_string(with_ui) + ")");

    // Leave the workspace the way a user does: open something else.
    s.app.open_asset(AssetType::Mesh, "meshes/cube.yaml");
    tick(s.engine, 4);

    coopa::test::require(s.app.active_asset_type() != AssetType::UI, "the workspace was left");
    coopa::test::expect(s.engine.overlay_layers().size() == base,
           "the preview layer went with it (" + std::to_string(base) + " expected, got " +
               std::to_string(s.engine.overlay_layers().size()) +
               "). A layer left behind keeps the panel painted over every later workspace");

    // And coming back brings it straight back, rather than needing the file reopened.
    s.app.open_asset(AssetType::UI, "ui/overlay_probe.yaml");
    tick(s.engine, 4);
    coopa::test::expect(s.engine.overlay_layers().size() == base + 1,
           "returning to the workspace shows the preview again without reopening the file");
}

/**
 * @brief Switching between the two editors rebuilds the preview, and the preview never takes
 *        the pointer unless Interact is on.
 *
 * Both halves of a bug found by hand. `ensure_preview_()` used to early-return on "a preview
 * exists" without checking it was the right KIND, so opening a panel after a placement file
 * reused the placement scene: the screen kept drawing and the panel never appeared. And the
 * preview layer was added with `input=true` unconditionally -- the engine reads the LAYER's
 * flag (`Engine::overlay_layer_placement_`), not the placement handed to
 * `set_scene_ui_placement()` -- so a full-screen screen like DemoHud swallowed every click
 * meant for the editor.
 */
COOPA_TEST(switching_between_the_editors_rebuilds_the_preview_and_never_steals_the_pointer) {
    EditorSession s;
    const std::filesystem::path tune = s.project.assets() / "ui" / "demo_hud.tune.yaml";
    const std::filesystem::path panel = s.project.assets() / "ui" / "probe2.yaml";
    write(tune, "# empty\n");
    write(panel, "title: Probe\nanchor: Center\nsize: [300, 200]\n"
                 "rows:\n  - { kind: label, label: Hello }\n");
    s.project.refresh();

    UiCanvasPanel& ui = s.app.ui_workspace();
    (void)tune;

    // Through the app every time: the preview is only ATTACHED while the UI workspace is the
    // active asset type (update_scene_ui_placement_ -> set_preview_visible), so opening the
    // panel directly leaves it detached and every pointer check below passes for the wrong
    // reason.
    auto open = [&](const char* rel) {
        s.app.open_asset(AssetType::UI, rel);
        tick(s.engine, 6);
        coopa::test::require(s.app.active_asset_type() == AssetType::UI,
                             std::string("the UI workspace opened for ") + rel);
    };

    // Placement first: the screen component runs, and no PanelScreen exists.
    open("ui/demo_hud.tune.yaml");
    coopa::test::expect(ui.mode() == UiCanvasPanel::Mode::Placement, "it opened Placement");
    coopa::test::require(ui.placed_screen() != nullptr, "the screen is running");
    coopa::test::expect(ui.panel_screen() == nullptr, "and there is no panel preview");

    // Now a panel. The placement preview must NOT be reused -- this is the bug: reusing it
    // left DemoHud drawing while the panel never appeared.
    open("ui/probe2.yaml");
    coopa::test::expect(ui.mode() == UiCanvasPanel::Mode::Panel, "the editor switched to Panel");
    coopa::test::expect(ui.placed_screen() == nullptr,
                        "the placement screen is gone -- it would otherwise keep drawing");
    coopa::test::require(ui.panel_screen() != nullptr, "a PanelScreen preview was built");
    coopa::test::expect(ui.panel_screen()->asset() != nullptr, "and it is showing the panel");

    // And back again, which is the direction that already worked -- pinned so it stays working.
    open("ui/demo_hud.tune.yaml");
    coopa::test::expect(ui.panel_screen() == nullptr, "the panel preview is gone");
    coopa::test::expect(ui.placed_screen() != nullptr, "the screen is running again");

    // The pointer. The layer's own input flag is what the engine reads, so this is the check
    // that a full-screen preview cannot swallow clicks meant for the editor.
    coopa::test::expect(!ui.preview_takes_input(),
                        "the preview does not take the pointer by default");
    ui.set_ui_interact(true);
    tick(s.engine, 3);
    coopa::test::expect(ui.preview_takes_input(), "Interact hands it the pointer");
    ui.set_ui_interact(false);
    tick(s.engine, 3);
    coopa::test::expect(!ui.preview_takes_input(), "and leaving Interact takes it back");
}

}  // namespace toy::editor::testing
