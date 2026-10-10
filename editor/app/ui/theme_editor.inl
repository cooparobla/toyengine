// editor/app/ui/theme_editor.inl -- included inside EditorApp's class body.
//
// The Theme editor: game UI themes (assets/ui/themes/*.yaml), the files a built game's themed
// widgets -- Window, MenuList, StatBar, SettingRow... -- take their colours, fonts and sizes
// from (uicoopa's builder theme; see ui_theme_schema.h). The editor's own chrome themes
// (editor/themes/) are not these and are not edited here.
//
//   Open      from the Themes tab of the asset browser, or a UI's Canvas tab ("Edit Theme").
//             The theme opens in the Properties editor's Theme tab beside a UI asset it is
//             previewed on -- the open UI, else the last one used, else ui/settings.
//   Edit      every section of the file (panels, text, buttons, sliders ... HUD, metrics) and its
//             fonts; edits are undoable (Ctrl+Z over the Theme tab) and stay in memory until
//             Save Theme (Ctrl+S saves everything).
//   Preview   the UI is rebuilt with the edited theme in place of its own: the unsaved theme is
//             written under <project>/.toyeditor/theme_preview/ at the theme's (and the UI's
//             theme's) asset path, with font paths made absolute, and that folder is put ahead
//             of assets/ in the scene loader's search roots for the rebuild -- the Theme
//             component resolves its `source` there. Nothing under assets/ changes until saved.

public:
    /** @brief Opens a game UI theme in the Theme tab, previewed on a UI asset. */
    bool open_theme(const fs::path& path);

    /**
     * @brief Creates ui/themes/<name>.yaml -- a copy of the open theme (unsaved edits included),
     *        else of the default one -- and opens it.
     */
    bool create_theme(const std::string& name);

    bool save_theme();

    /** @brief Applies `fn` to the open theme's document as one undoable edit, and re-previews. */
    void edit_theme(const std::string& label, const std::function<void(Node&)>& fn, const std::string& merge_key = {});

    // --- queries for tests ---
    ThemeDocument& theme_document() { return game_theme_; }
    /** @brief The folder an open theme's unsaved edits are previewed from. */
    fs::path theme_preview_root() const { return project_.root() / ".toyeditor" / "theme_preview"; }

private:
    std::string theme_preview_ui_;   ///< The UI asset (assets-relative) the theme was last previewed on.

    /** @brief The UI asset to preview a theme on: the last used, else ui/settings, else any. */
    std::string theme_preview_target_();

    void theme_changed_() { queue_rebuild_(); }

    /** @brief The open UI's root Theme `source` (assets-relative), or empty. */
    std::string ui_theme_source_() const;

    /** @brief The open theme as the preview writes it: font paths made absolute (the copy lives elsewhere). */
    Node theme_preview_node_() const;

    /**
     * @brief Before every rebuild: while a theme is open beside a UI, shadow its file (and the
     *        UI's own theme file) with the edited theme -- see this file's header. Otherwise
     *        the scene loader searches the Engine's asset roots (the project's assets/, then
     *        the engine checkout's -- where engine-shipped object assets such as the weather
     *        effects live), exactly as the game does.
     */
    void apply_theme_preview_();

    // =================================================================================
    // The Theme tab
    // =================================================================================

    void draw_theme_props_(imm::Context& ctx);

    /**
     * @brief The theme's fonts: the default face (`text.font_path`) and one per role
     *        (`text.fonts.<role>`, inheriting the default when unset). Paths are stored relative
     *        to the theme file, as the format expects; the list offers the project's fonts/.
     */
    void draw_theme_fonts_(imm::Context& ctx);

    /** @brief Points the open UI's root Theme component at `source` (undoable). */
    void ui_use_theme_(const std::string& source);
