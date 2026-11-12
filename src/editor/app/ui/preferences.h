/**
 * @file preferences.h
 * @brief Edit > Preferences: the editor's own settings, per user (~/.toyengine_editor.yaml, next
 *        to the recent projects), never per project. A category list on the left, the category's
 *        settings on the right; every edit applies at once and is saved when the mouse lets go.
 *
 * Interface    theme (tiles with a preview of each), interface scale, tooltip delay
 * Viewport     field of view, gizmo size, grid, ambient occlusion, isolate in Edit Mode
 * Navigation   orbit / zoom / fly speeds, wheel direction, what a trackpad swipe does
 * Snapping     the Ctrl-snap increments and grid-adaptive move snapping
 * Assets       engine assets in the Asset panel, the default sort
 * Keymap       every shortcut, searchable (also Help > Controls)
 *
 * The numeric / toggle preferences are rows of a table (prefs_table_()) pointing at the live
 * members they drive; the few older ones (theme, AO, isolate, asset panel) keep their own
 * setters, which save themselves.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_PREFERENCES_H
#define TOYEDITOR_APP_UI_PREFERENCES_H

#include "editor/app/editor_panel.h"

namespace toy::editor {

/** @brief See the file doc. */
class PreferencesPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    /** @brief One numeric or toggle preference, pointing at the member it drives. */
    struct PrefRow {
        const char* key;        ///< In ~/.toyengine_editor.yaml.
        const char* category;
        const char* label;
        const char* tip;
        float* f = nullptr;     ///< A number...
        bool* b = nullptr;      ///< ...a toggle...
        int* i = nullptr;       ///< ...or an index into `choices`, stored in the file by its name.
        float def = 0.0f, lo = 0.0f, hi = 1.0f;
        const char* fmt = "%.2f";
        std::vector<std::string> choices;   ///< A toggle's two-way choice (false, true), or an index's names.
        bool drag = false;                  ///< A drag field (unbounded feel) instead of a slider.
    };

    std::vector<PrefRow> prefs_table_();

    /** @brief Reads the table's preferences from `prefs` (missing keys keep their defaults) and applies them. */
    void prefs_load_(const Node& prefs);
    /** @brief Pushes preference members into what they drive (the rest read them each frame). */
    void prefs_apply_();
    /** @brief Writes the table's preferences into ~/.toyengine_editor.yaml (keeping every other key). */
    void prefs_save_();

public:
    /** @brief Opens Edit > Preferences on `category`. */
    void open_preferences(const std::string& category = "Interface");
    /**
     * @brief Sets one table preference ("orbit_speed", "invert_zoom"...) as the window would,
     *        applies and saves it. False for an unknown key.
     */
    bool set_preference(const std::string& key, const Node& value);
private:

    // --- the window ----------------------------------------------------------------------

    struct ThemeSwatch { std::string id, name; glm::vec4 bg, panel, header, accent, text, dim, button; };

    bool prefs_dirty_ = false;            ///< A preference changed; written when the mouse lets go.
    std::string prefs_category_ = "Interface";
    std::string prefs_keymap_filter_;
    std::vector<ThemeSwatch> prefs_themes_;   ///< Theme tiles, read while the window is open.

    /** @brief The theme tiles' colours, read once per window opening. */
    const std::vector<ThemeSwatch>& prefs_theme_swatches_();

    /** @brief A titled block inside the pane: heading, a dim line saying what it's for, a rule. */
    void prefs_section_(imm::Context& ctx, const std::string& title, const std::string& what);

    /** @brief A slider filling `b` (the property column, minus the reset arrow). */
    bool prefs_slider_(imm::Context& ctx, const imm::Box& b, float* v, float lo, float hi, const char* fmt);

    /** @brief The table's rows for `category`, each with a reset arrow while it differs from its default. */
    void prefs_rows_(imm::Context& ctx, const std::string& category);

    void draw_preferences_modal_(imm::Context& ctx);

    // --- keymap --------------------------------------------------------------------------

    struct KeyBinding { const char* action; const char* keys; };
    struct KeyGroup { const char* title; std::vector<KeyBinding> keys; };

    static const std::vector<KeyGroup>& keymap_();

    /** @brief The keymap as a searchable table: action on the left, key chips on the right. */
    void draw_keymap_table_(imm::Context& ctx, std::string& filter);
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_PREFERENCES_H
