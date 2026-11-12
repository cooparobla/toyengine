/**
 * @file file_dialog_test.cpp
 * @brief uicoopa's file picker (the editor's and the hub's), driven through real frames: type-to-select,
 * the format filter, Escape, folder picking, overwrite confirmation and extension completion.
 */

#include <coopa/testing/test.h>

#include "editor/support/imm_harness.h"
#include <uicoopa/immediate/imm_file_dialog.h>

#include "editor/support/view_helpers.h"

COOPA_TEST_SUITE("file_dialog");

namespace toy::editor::testing {

COOPA_TEST(open_filter_folder_and_save_flows) {
    // uicoopa's picker (the editor's and the hub's), driven through real frames.
    using coopa::input::Key;
    using coopa::input::KeyAction;
    using coopa::input::KeyEvent;
    using FD = coopa::ui::imm::FileDialog;
    const fs::path dir = coopa::test::scratch_dir("file_dialog");
    fs::create_directories(dir / "sub");
    for (const char* f : {"a.yaml", "b.png", "c.txt"}) write_text(dir / f, "x");
    ImmHarness h;
    FD fd;
    auto ui = [&](coopa::ui::imm::Context& c) { fd.draw(c); };
    const auto enter = std::vector<KeyEvent>{{Key::Enter, 0, KeyAction::Press, coopa::input::Mods::None}};

    fs::path got;
    fd.open(h.ctx, FD::Mode::OpenFile, "Open", dir, {".yaml"}, [&](const fs::path& p) { got = p; });
    h.frame(ui);
    h.frame(ui);
    expect(fd.is_open(), "open() shows the picker from the next frame");
    h.frame(ui, {'a'});                 // type-to-select
    h.frame(ui, {}, enter);
    expect(got == dir / "a.yaml" && !fd.is_open(), "typing a name selects it and Enter opens it");

    got.clear();
    fd.open(h.ctx, FD::Mode::OpenFile, "Open", dir, {".yaml"}, [&](const fs::path& p) { got = p; });
    h.frame(ui);
    h.frame(ui, {'b'});
    h.frame(ui, {}, enter);
    expect(got.empty() && fd.is_open(), "a file the Format filter rejects can't be opened");
    h.frame(ui, {}, {{Key::Escape, 0, KeyAction::Press, coopa::input::Mods::None}});
    expect(!fd.is_open() && got.empty(), "Escape cancels");

    got.clear();
    fd.open(h.ctx, FD::Mode::PickFolder, "Choose", dir, {}, [&](const fs::path& p) { got = p; });
    h.frame(ui);
    h.frame(ui, {'s'});
    h.frame(ui, {}, enter);
    expect(got == dir / "sub", "PickFolder chooses the selected folder");

    got.clear();
    fd.open(h.ctx, FD::Mode::SaveFile, "Save", dir, {".yaml"}, [&](const fs::path& p) { got = p; }, "a.yaml");
    h.frame(ui);
    h.frame(ui);
    h.frame(ui, {}, enter);
    expect(got.empty() && fd.is_open(), "saving over an existing file asks first");
    h.frame(ui, {}, enter);
    expect(got == dir / "a.yaml", "...and the second press replaces it");

    std::vector<fs::path> used;
    fd.on_folder_used = [&](const fs::path& p, FD::Mode) { used.push_back(p); };
    fd.open(h.ctx, FD::Mode::SaveFile, "Save", dir / "sub", {".yaml"}, [&](const fs::path& p) { got = p; }, "fresh");
    h.frame(ui);
    h.frame(ui);
    h.frame(ui, {}, enter);
    expect(got == dir / "sub" / "fresh.yaml", "Save adds the format's extension to a bare name");
    expect(used.size() == 1 && used[0] == dir / "sub", "on_folder_used reports the folder the dialog finished in");
}

} // namespace toy::editor::testing
