/**
 * @file project_settings.h
 * @brief Edit > Project Settings: the project's own settings (assets/config.yaml) in one modal --
 *        the render settings by category, Output (window, jobs, debug) and Physics. These are the
 *        defaults every scene starts from; a scene's Properties tabs (Render, World, Scene >
 *        Physics) edit only that scene's overrides of them. Rows here always write config.yaml
 *        (project_settings_mode_ turns the scene layer off while the modal draws).
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_PROJECT_SETTINGS_H
#define TOYEDITOR_APP_UI_PROJECT_SETTINGS_H

#include "editor/app/editor_panel.h"

namespace toy::editor {

/** @brief See the file doc. */
class ProjectSettingsPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access
    /// level they had inside EditorApp's body.
    friend class EditorApp;

    /** @brief Opens Project Settings on `category` ("General", "Render Features", "Output"...). */
    void open_project_settings_(const std::string& category);

    /** @brief Applies settings fixed at pipeline construction now, in place (the window stays open). */
    void rebuild_renderer_();

    /** @brief The modal's categories: the render settings' own, then Output and Physics. */
    std::vector<std::string> project_settings_categories_();

    /** @brief config.yaml render keys the schema doesn't know (a typo, or from a newer engine). */
    bool has_unrecognized_render_keys_();

    void draw_project_settings_modal_(imm::Context& ctx);
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_PROJECT_SETTINGS_H
