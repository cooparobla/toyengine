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
 * The file is named after the screen (`DemoHud.tune.yaml`), which is the whole binding -- there
 * is no "which screen is this for" field to get wrong. The boxes cannot come from the file,
 * because a file that has never been written states none: they come from the screen DECLARING
 * them as it draws, which is why this needs the component to actually run.
 */
COOPA_TEST(a_placement_file_opens_the_box_editor_and_learns_the_screens_boxes) {
    EditorSession s;
    const std::filesystem::path tune = s.project.assets() / "ui" / "DemoHud.tune.yaml";
    write(tune, "# empty: every box keeps the placement its own code states\n");
    s.project.refresh();

    UiCanvasPanel& ui = s.app.ui_workspace();
    coopa::test::expect(UiCanvasPanel::is_placement_file(tune), "the suffix marks it as a placement file");
    coopa::test::require(ui.open_ui_asset(tune), "the placement file opened");
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
    reloaded.load("ui/DemoHud");
    const toy::ui::Tunable* e = reloaded.find(name);
    coopa::test::require(e != nullptr, "the saved entry reloads");
    coopa::test::expect_near(e->pos.x, before.x + 40.0f, 0.5f, "the x it was moved to survived the round trip");
    coopa::test::expect_near(e->pos.y, before.y + 24.0f, 0.5f, "and the y");
}

}  // namespace toy::editor::testing
