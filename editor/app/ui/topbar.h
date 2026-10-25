/**
 * @file topbar.h
 * @brief Blender's top bar: File / Edit / Render / Window / Help, then the workspace tabs
 *        (Layout, Modeling, Shading). Unity's Play / Pause / Step sit centred; the scene name at
 *        the right.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_TOPBAR_H
#define TOYEDITOR_APP_UI_TOPBAR_H

#include "editor/app/editor_panel.h"

// Types this panel's declarations name (pre-seeded from the repo's type index;
// prune anything the declarations do not actually need).
#include <uicoopa/immediate/imm_theme.h>

namespace toy::editor {

/** @brief See the file doc. */
class TopbarPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    void draw_topbar_(imm::Context& ctx, const imm::Box& b);

    void draw_topbar_project_(imm::Context& ctx, const imm::Box& b);

    void draw_file_menu_(imm::Context& ctx);

    void draw_edit_menu_(imm::Context& ctx);

    /** @brief Edit > Theme: every theme in editor/themes, the active one checked. */
    void draw_theme_menu_(imm::Context& ctx);

    /** @brief Writes the active theme with every role spelled out (inherited ones included). */
    void export_theme_();

    void draw_render_menu_(imm::Context& ctx);

    void draw_window_menu_(imm::Context& ctx);

    // --- Unity play controls ---

    void toggle_pause_();

    /** @brief One simulated frame of a paused game (starts play paused if not playing). */
    void step_simulation_();

    /** @brief F12: writes the current full render (low-res scene image) to <project>/renders/. */
    void render_image_();

    // =================================================================================
    // Branding: the logo (toyengine/core/branding.h) as vector triangles, and About
    // =================================================================================

    /** @brief Draws the toyengine logo into `box` (kept square, centred) -- see editor/ui/logo.h. */
    void draw_logo_(imm::Context& ctx, const imm::Box& box);

    /** @brief The About box's facts, as (label, value) rows -- also what "Copy" puts on the clipboard. */
    std::vector<std::pair<std::string, std::string>> about_rows_();

    void draw_about_modal_(imm::Context& ctx);
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_TOPBAR_H
