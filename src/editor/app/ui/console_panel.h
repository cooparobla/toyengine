/**
 * @file console_panel.h
 * @brief The Console: the editor's log, with severity icons and per-severity filters.
 *
 * Shown under the viewer. (The *asset* browser is a different panel -- see ui/assets.cpp.)
 *
 * The reference for the panel shape this directory is migrating to: one class per panel,
 * owning its own state, reaching the editor only through EditorContext. `EditorApp` holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not all 25 panels. See editor/app/README.md.
 */

#ifndef TOYEDITOR_APP_UI_CONSOLE_PANEL_H
#define TOYEDITOR_APP_UI_CONSOLE_PANEL_H

#include <uicoopa/immediate/imm.h>

namespace toy {
namespace editor {

namespace imm = coopa::ui::imm;

class EditorContext;

/** @brief The Console panel. */
class ConsolePanel {
public:
    explicit ConsolePanel(EditorContext& ed) : ed_(ed) {}

    /**
     * @brief Draws the toolbar into `hb` and the message list into `body`.
     * @param hb   Header strip: the severity filters and Clear.
     * @param body The scrolling message region.
     */
    void draw(imm::Context& ctx, const imm::Box& hb, const imm::Box& body);

private:
    EditorContext& ed_;
    /// Per-severity visibility (info / warning / error) -- panel-private, which is the point.
    bool show_[3] = {true, true, true};
};

} // namespace editor
} // namespace toy

#endif // TOYEDITOR_APP_UI_CONSOLE_PANEL_H
