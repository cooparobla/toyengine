#include <toyengine/ui/panel_screen.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <coopa/asset/asset_manager.h>

#include <uicoopa/text/font_defaults.h>

namespace toy::ui {

void PanelScreen::on_ready() {
    // Registered before the early return below, so a host that supplied the panel through
    // set_panel() -- the editor previewing one -- still gets a working Close button. A game
    // that wants different behaviour calls on_action("close", ...) itself; the map is
    // last-write-wins, and on_ready() runs at the first command flush, after every sibling's
    // start(), so a handler registered from a sibling's start() would be overwritten. Register
    // from on_ready() or later.
    bindings_.on_action("close", [this] { hidden = true; });

    // A host that called set_panel() before this screen started owns what is shown.
    if (panel_ || panel.empty()) return;

    // Through the asset system when a host has published one (set_panel_assets()): the file
    // is then parsed once however many screens name it, and an edit hot-reloads straight into
    // the editor's preview. load() is synchronous for a few kilobytes of YAML, so the panel is
    // still there on the first frame this screen draws.
    // `.yaml` is optional in a scene, so a screen can say `panel: ui/settings`. Both paths
    // below want the real filename.
    const bool has_ext = panel.size() > 5 && panel.rfind(".yaml") == panel.size() - 5;
    const std::string rel = has_ext ? panel : panel + ".yaml";

    if (coopa::asset::AssetManager* assets = panel_assets()) {
        handle_ = assets->load<UiPanel>(rel);
        if (handle_.get()) return;
        std::fprintf(stderr, "[toy::ui] PanelScreen: panel '%s' did not load\n", rel.c_str());
        load_failed_ = true;
        return;
    }

    // No host: read the file directly. This is the path the headless unit tests take.
    std::string found;
    for (const std::string& root : coopa::ui::FontDefaults::search_dirs) {
        const std::filesystem::path p = std::filesystem::path(root) / rel;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            found = p.string();
            break;
        }
    }
    if (found.empty()) {
        std::fprintf(stderr, "[toy::ui] PanelScreen: panel '%s' not found\n", panel.c_str());
        load_failed_ = true;
        return;
    }
    try {
        std::ifstream f(found);
        std::ostringstream ss;
        ss << f.rdbuf();
        panel_ = std::make_shared<UiPanel>(parse_panel(ss.str()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[toy::ui] PanelScreen: '%s' failed to parse (%s)\n", panel.c_str(), e.what());
        load_failed_ = true;
        return;
    }
}

void PanelScreen::draw(coopa::ui::imm::Context& ctx) {
    const UiPanel* p = asset();
    if (!p) {
        if (load_failed_) {
            // Say so on screen rather than drawing nothing: a blank panel looks like a
            // layout bug, and the file is the thing to go and fix.
            ctx.label_dim("Panel '" + panel + "' did not load.");
        }
        return;
    }
    // The panel asset owns its own geometry: a tune entry names this screen's BOX, and for an
    // authored panel that box is the asset's anchor/offset/size. Letting a tune file override
    // them would mean editing `size:` in the YAML silently did nothing -- including in the
    // editor preview, which is the author's only feedback loop.
    const coopa::ui::imm::Box b = ctx.anchor(p->anchor, p->offset, p->size);
    last_action_ = draw_panel(ctx, b, *p, bindings_, &tab_);
}

}  // namespace toy::ui
