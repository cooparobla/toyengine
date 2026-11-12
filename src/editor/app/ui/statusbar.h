/**
 * @file statusbar.h
 * @brief Blender's status bar: mouse-button hints for the current context at the left, the latest
 *        report in the middle, scene statistics and the version at the right.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_STATUSBAR_H
#define TOYEDITOR_APP_UI_STATUSBAR_H

#include "editor/app/editor_panel.h"

namespace toy::editor {

/** @brief See the file doc. */
class StatusBarPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access
    /// level they had inside EditorApp's body.
    friend class EditorApp;

    void draw_statusbar_(imm::Context& ctx, const imm::Box& b);

    /** @brief "Scene | Objects 1/4 | Verts 8 | Faces 6 | Tris 12" (edit mode: selected/total). */
    std::string scene_stats_();
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_STATUSBAR_H
