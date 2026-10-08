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
    bool open_theme(const fs::path& path) {
        const std::string ref = strip_yaml_ext(short_ref(project_.relative(path)));
        try {
            game_theme_.load(path, ref);
        } catch (const std::exception& e) {
            log_error(std::string("Open theme failed: ") + e.what());
            return false;
        }
        if (active_type_ != AssetType::UI) {
            const std::string ui = theme_preview_target_();
            if (!ui.empty()) open_ui_asset(project_.absolute(ui));
            else log_info("No UI asset to preview the theme on -- create one in the UI tab");
        } else {
            queue_rebuild_();
        }
        prop_tab_ = PropTab::Theme;
        asset_tab_ = AssetType::Theme;
        log_info("Opened theme " + ref);
        return true;
    }

    /**
     * @brief Creates ui/themes/<name>.yaml -- a copy of the open theme (unsaved edits included),
     *        else of the default one -- and opens it.
     */
    bool create_theme(const std::string& name) {
        ui_install_template_themes_();
        const std::string dir = new_asset_dir_("ui/themes");
        const std::string n = unique_asset_name_(dir, name);
        const fs::path dst = project_.assets() / dir / (n + ".yaml");
        Node t = Node::mapping();
        try {
            if (game_theme_.open()) t = game_theme_.node;
            else if (coopa::yaml::document_exists(project_.assets() / "ui" / "themes" / "default.yaml"))
                t = coopa::yaml::load_document(project_.assets() / "ui" / "themes" / "default.yaml");
            if (!t.is_mapping()) t = Node::mapping();
            t["name"] = Node(n);
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            coopa::yaml::save_document(dst, t);
            project_.refresh();
        } catch (const std::exception& e) {
            log_error(std::string("Create theme failed: ") + e.what());
            return false;
        }
        return open_theme(dst);
    }

    bool save_theme() {
        if (!game_theme_.open()) return false;
        try {
            game_theme_.save();
            log_info("Saved theme " + game_theme_.ref);
            queue_rebuild_();
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Save theme failed: ") + e.what());
            return false;
        }
    }

    /** @brief Applies `fn` to the open theme's document as one undoable edit, and re-previews. */
    void edit_theme(const std::string& label, const std::function<void(Node&)>& fn, const std::string& merge_key = {}) {
        if (!game_theme_.open()) return;
        const Node before = game_theme_.node;
        fn(game_theme_.node);
        if (game_theme_.node == before) return;
        game_theme_.commit(label, before, merge_key);
        theme_changed_();
    }

    // --- queries for tests ---
    ThemeDocument& theme_document() { return game_theme_; }
    /** @brief The folder an open theme's unsaved edits are previewed from. */
    fs::path theme_preview_root() const { return project_.root() / ".toyeditor" / "theme_preview"; }

private:
    std::string theme_preview_ui_;   ///< The UI asset (assets-relative) the theme was last previewed on.

    /** @brief The UI asset to preview a theme on: the last used, else ui/settings, else any. */
    std::string theme_preview_target_() {
        const auto uis = list_assets_(AssetType::UI);
        auto has = [&](const std::string& r) { return std::find(uis.begin(), uis.end(), r) != uis.end(); };
        if (!theme_preview_ui_.empty() && has(theme_preview_ui_)) return theme_preview_ui_;
        for (const auto& r : uis) if (fs::path(r).stem() == "settings") return r;   // ui/<tags>/settings
        return uis.empty() ? std::string() : uis.front();
    }

    void theme_changed_() { queue_rebuild_(); }

    /** @brief The open UI's root Theme `source` (assets-relative), or empty. */
    std::string ui_theme_source_() const {
        if (active_type_ != AssetType::UI) return {};
        const ObjectId root = doc_.object_root();
        const int ti = root ? doc_.find_component(root, "Theme") : -1;
        return ti >= 0 ? get_string(doc_.find(root)->at("components").as_seq()[static_cast<size_t>(ti)], "source") : std::string();
    }

    /** @brief The open theme as the preview writes it: font paths made absolute (the copy lives elsewhere). */
    Node theme_preview_node_() const {
        Node t = game_theme_.node;
        if (!t.is_mapping() || !t.contains("text") || !t.at("text").is_mapping()) return t;
        const fs::path dir = game_theme_.path.parent_path();
        auto absolutize = [&](Node& n) {
            if (!n.is_string()) return;
            const std::string p = n.get_value<std::string>();
            if (!p.empty() && !fs::path(p).is_absolute()) n = Node((dir / p).lexically_normal().generic_string());
        };
        Node& text = t["text"];
        if (text.contains("font_path")) absolutize(text["font_path"]);
        if (text.contains("fonts") && text.at("fonts").is_mapping()) {
            for (const auto& role : ui_theme_font_roles()) {
                if (text["fonts"].contains(role) && text["fonts"][role].is_mapping() && text["fonts"][role].contains("path"))
                    absolutize(text["fonts"][role]["path"]);
            }
        }
        return t;
    }

    /**
     * @brief Before every rebuild: while a theme is open beside a UI, shadow its file (and the
     *        UI's own theme file) with the edited theme -- see this file's header. Otherwise
     *        the scene loader searches the Engine's asset roots (the project's assets/, then
     *        the engine checkout's -- where engine-shipped object assets such as the weather
     *        effects live), exactly as the game does.
     */
    void apply_theme_preview_() {
        std::error_code ec;
        const fs::path root = theme_preview_root();
        fs::remove_all(root, ec);
        std::vector<std::string> roots = engine_.asset_roots();
        if (game_theme_.open() && active_type_ == AssetType::UI) {
            theme_preview_ui_ = project_.relative(active_path_);
            const Node t = theme_preview_node_();
            std::vector<std::string> refs = {game_theme_.ref + ".yaml"};
            const std::string own = ui_theme_source_();
            if (!own.empty() && own != refs.front() && !fs::path(own).is_absolute()) refs.push_back(own);
            try {
                for (const auto& ref : refs) {
                    const fs::path p = root / ref;
                    fs::create_directories(p.parent_path(), ec);
                    coopa::yaml::save_document(p, t);
                }
                roots.insert(roots.begin(), root.string());
            } catch (const std::exception& e) {
                log_error(std::string("Theme preview: ") + e.what());
            }
        }
        coopa::scene::SceneLoader::set_search_roots(roots);
    }

    // =================================================================================
    // The Theme tab
    // =================================================================================

    void draw_theme_props_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!game_theme_.open()) {
            ctx.label_dim("No theme open. Open one from the asset browser's Themes tab.");
            return;
        }
        ctx.label_dim(game_theme_.ref + (game_theme_.dirty() ? "  (modified)" : ""));
        if (ctx.button("Save Theme", 120, true, I::Save)) save_theme();
        ctx.same_line();
        if (ctx.button("Duplicate", 110, true, I::Duplicate)) {
            const std::string base = fs::path(game_theme_.ref).filename().string() + "_copy";
            deferred_.push_back([this, base] { create_theme(base); });
        }
        ctx.tooltip("Duplicate\nCopies this theme (unsaved edits included) to a new file and opens it");
        ctx.spacing(4);

        // What it previews on, and whether that UI uses it.
        if (ctx.collapsing_header("Preview", true, nullptr, I::UiCanvas)) {
            ctx.indent(4);
            const auto uis = list_assets_(AssetType::UI);
            const std::string cur = active_type_ == AssetType::UI ? project_.relative(active_path_) : std::string();
            int idx = -1;
            std::vector<std::string> names;
            for (size_t i = 0; i < uis.size(); ++i) {
                names.push_back(uis[i]);
                if (uis[i] == cur) idx = static_cast<int>(i);
            }
            if (idx < 0) { names.push_back(cur.empty() ? std::string("(no UI open)") : cur); idx = static_cast<int>(names.size()) - 1; }
            const int before = idx;
            if (ctx.combo("Preview on", &idx, names) && idx != before && idx < static_cast<int>(uis.size())) {
                const std::string ui = uis[static_cast<size_t>(idx)];
                if (doc_.dirty()) log_error("Save " + cur + " before previewing on another UI");
                else deferred_.push_back([this, ui] { open_ui_asset(project_.absolute(ui)); prop_tab_ = PropTab::Theme; });
            }
            ctx.tooltip("Preview on\nThe UI asset shown with this theme while you edit it (save the open UI first)");
            const std::string own = ui_theme_source_();
            const std::string mine = game_theme_.ref + ".yaml";
            if (active_type_ == AssetType::UI && own != mine) {
                ctx.label_dim(own.empty() ? "Previewed in place of the built-in theme." : "Previewed in place of " + own + ".");
                if (ctx.button("Use This Theme in " + fs::path(cur).stem().string(), -1, true, I::Link)) ui_use_theme_(mine);
                ctx.tooltip("Use This Theme\nSets the UI's Theme component to this file");
            } else if (active_type_ == AssetType::UI) {
                ctx.label_dim("This UI uses this theme.");
            }
            ctx.unindent(4);
            ctx.spacing(4);
        }

        draw_theme_fonts_(ctx);
        const InspectorEnv env = inspector_env_();
        for (const auto& section : ui_theme_sections()) {
            if (!ctx.collapsing_header(section.title, section.key == "panel" || section.key == "text" || section.key == "button")) continue;
            ctx.indent(4);
            ctx.push_id(section.key);
            Node view = game_theme_.node.contains(section.key) && game_theme_.node.at(section.key).is_mapping() ? game_theme_.node.at(section.key)
                                                                                                   : Node::mapping();
            const EditResult r = draw_fields(ctx, section.fields, view, env, false, {"font_path", "fonts"});
            if (r.changed) {
                edit_theme("Edit " + section.key + "." + r.key, [&](Node& t) { t[section.key] = view; },
                           r.active ? "t:" + section.key + "." + r.key : std::string());
            }
            if (r.finished) game_theme_.undo.end_merge();
            ctx.pop_id();
            ctx.unindent(4);
            ctx.spacing(4);
        }
    }

    /**
     * @brief The theme's fonts: the default face (`text.font_path`) and one per role
     *        (`text.fonts.<role>`, inheriting the default when unset). Paths are stored relative
     *        to the theme file, as the format expects; the list offers the project's fonts/.
     */
    void draw_theme_fonts_(imm::Context& ctx) {
        if (!ctx.collapsing_header("Fonts", false, nullptr, imm::Icon::UiText)) return;
        ctx.indent(4);
        const fs::path dir = game_theme_.path.parent_path();
        std::vector<std::string> fonts;   // assets-relative
        for (const auto& f : project_.list("fonts", "")) {
            const std::string e = fs::path(f).extension().string();
            if (e == ".ttf" || e == ".otf") fonts.push_back(f);
        }
        auto to_rel = [&](const std::string& asset_rel) {
            return fs::relative(project_.absolute(asset_rel), dir).generic_string();
        };
        auto to_asset = [&](const std::string& stored) {
            if (stored.empty()) return std::string();
            return project_.relative((dir / stored).lexically_normal());
        };
        // One dropdown: "(inherit)" or a project font. Returns the new stored path ("" = inherit).
        auto font_row = [&](const std::string& label, const std::string& stored, const char* none) -> std::optional<std::string> {
            const std::string cur = to_asset(stored);
            std::vector<std::string> names = {none};
            int idx = 0;
            for (size_t i = 0; i < fonts.size(); ++i) {
                names.push_back(fs::path(fonts[i]).stem().string());
                if (fonts[i] == cur) idx = static_cast<int>(i) + 1;
            }
            if (idx == 0 && !stored.empty()) { names.push_back(stored + " (missing)"); idx = static_cast<int>(names.size()) - 1; }
            const int before = idx;
            if (!ctx.combo(label, &idx, names) || idx == before) return std::nullopt;
            if (idx == 0) return std::string();
            if (idx > static_cast<int>(fonts.size())) return std::nullopt;
            return to_rel(fonts[static_cast<size_t>(idx) - 1]);
        };
        const Node text = game_theme_.node.contains("text") && game_theme_.node.at("text").is_mapping() ? game_theme_.node.at("text") : Node::mapping();
        if (auto v = font_row("Default##font_path", get_string(text, "font_path"), "(built-in)")) {
            edit_theme("Edit text.font_path", [&](Node& t) {
                if (!t.contains("text") || !t.at("text").is_mapping()) t["text"] = Node::mapping();
                if (v->empty()) erase_key(t["text"], "font_path");
                else t["text"]["font_path"] = Node(*v);
            });
        }
        ctx.tooltip("Default Font\nEvery text role without a font of its own uses this face");
        const Node roles = text.contains("fonts") && text.at("fonts").is_mapping() ? text.at("fonts") : Node::mapping();
        for (const auto& role : ui_theme_font_roles()) {
            const Node r = roles.contains(role) && roles.at(role).is_mapping() ? roles.at(role) : Node::mapping();
            std::string label = role;
            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            if (auto v = font_row(label + "##font_" + role, get_string(r, "path"), "(default font)")) {
                edit_theme("Edit text.fonts." + role, [&](Node& t) {
                    if (!t.contains("text") || !t.at("text").is_mapping()) t["text"] = Node::mapping();
                    Node& tx = t["text"];
                    if (!tx.contains("fonts") || !tx.at("fonts").is_mapping()) tx["fonts"] = Node::mapping();
                    Node entry = tx["fonts"].contains(role) && tx["fonts"][role].is_mapping() ? tx["fonts"][role] : Node::mapping();
                    if (v->empty()) erase_key(entry, "path");
                    else entry["path"] = Node(*v);
                    if (entry.size() > 0) tx["fonts"][role] = entry;
                    else erase_key(tx["fonts"], role);
                });
            }
        }
        ctx.unindent(4);
        ctx.spacing(4);
    }

    /** @brief Points the open UI's root Theme component at `source` (undoable). */
    void ui_use_theme_(const std::string& source) {
        const ObjectId root = doc_.object_root();
        if (!root) return;
        const int ti = doc_.find_component(root, "Theme");
        Node comp = Node::mapping();
        comp["type"] = Node(std::string("Theme"));
        comp["source"] = Node(source);
        if (ti >= 0) apply_(doc_.set_component(root, ti, comp, "Use Theme"));
        else apply_(doc_.add_component(root, comp));
        queue_rebuild_();
    }
