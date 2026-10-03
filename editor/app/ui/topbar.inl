// editor/app/ui/topbar.inl -- included inside EditorApp's class body.
//
// Blender's top bar: File / Edit / Render / Window / Help, then the workspace tabs (Layout,
// Modeling, Shading). Unity's Play / Pause / Step sit centred; the scene name at the right.

    void draw_topbar_(imm::Context& ctx, const imm::Box& b) {
        using I = imm::Icon;
        ctx.fill(b, glm::vec4(0.137f, 0.137f, 0.137f, 1.0f));   // #232323
        const char* menus[] = {"File", "Edit", "Render", "Window", "Help"};
        float menus_w = 8;
        for (const char* m : menus) menus_w += ctx.text_width(m) + ctx.style.padding * 3;
        // App badge (Blender's logo slot).
        ctx.icon(I::Cube, {b.x + 6, b.y + 5, b.h - 10, b.h - 10}, ctx.style.object_active);
        const imm::Box mb{b.x + b.h, b.y, menus_w, b.h};
        ctx.begin_menubar(mb);
        draw_file_menu_(ctx);
        draw_edit_menu_(ctx);
        draw_render_menu_(ctx);
        draw_window_menu_(ctx);
        if (ctx.begin_menu("Help")) {
            if (ctx.menu_item("Controls...", "", nullptr, true, I::Keyboard)) pending_modal_ = "Controls";
            if (ctx.menu_item("About toyengine editor", "", nullptr, true, I::Info)) pending_modal_ = "About";
            ctx.end_menu();
        }
        ctx.end_menubar();

        // Workspace tabs.
        static const std::vector<std::string> ws = {"Layout", "Modeling", "Shading"};
        static const std::vector<I> ws_icons = {I::Asset, I::EditMode, I::ShadeMaterial};
        int t = static_cast<int>(tab_);
        if (ctx.tab_bar("workspaces", {mb.right() + 12, b.y, 360, b.h}, ws, &t, &ws_icons, false)) set_tab(static_cast<Tab>(t));

        // Unity play controls, centred.
        const float s = b.h - 6;
        const float cx = b.x + b.w * 0.5f - s * 1.5f;
        const bool paused = playing() && play_scene_ && !play_scene_->is_simulating() && step_countdown_ == 0;
        ctx.fill_rounded({cx - 2, b.y + 2, s * 3 + 4, s + 2}, glm::vec4(0.2f, 0.2f, 0.2f, 1.0f));
        if (ctx.icon_button("tb_play", playing() ? I::Stop : I::Play, playing() ? "Stop\nLeave play mode (F5 / Esc)" : "Play\nRun the scene in the game simulation (F5)",
                            playing(), s, imm::Context::kLeft, imm::Box{cx, b.y + 3, s, s})) {
            playing() ? stop() : play();
        }
        if (ctx.icon_button("tb_pause", I::Pause, "Pause\nFreeze the running simulation", paused, s, 0, imm::Box{cx + s, b.y + 3, s, s})) toggle_pause_();
        if (ctx.icon_button("tb_step", I::Step, "Step\nAdvance the paused simulation by one frame", false, s, imm::Context::kRight,
                            imm::Box{cx + s * 2, b.y + 3, s, s})) {
            step_simulation_();
        }

        // Scene name at the right (Blender's scene selector).
        const std::string scene = doc_.scene_name() + (doc_.dirty() ? " *" : "");
        const float sw = ctx.text_width(scene) + 40;
        const imm::Box sb{b.right() - sw - 8, b.y + 4, sw, b.h - 8};
        ctx.fill_rounded(sb, ctx.style.field);
        ctx.icon(I::Scene, {sb.x + 5, sb.y + 2, sb.h - 4, sb.h - 4}, ctx.style.text_dim);
        ctx.text_in({sb.x + sb.h + 4, sb.y, sb.w - sb.h - 4, sb.h}, scene, ctx.style.text, 0.0f);
    }

    void draw_file_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("File")) return;
        if (ctx.menu_item("New Scene", "Ctrl N", nullptr, true, I::File)) guarded_([this] { new_scene(); });
        if (ctx.menu_item("Open Scene...", "Ctrl O", nullptr, true, I::Folder)) guarded_([this] { open_scene_dialog_(); });
        if (ctx.begin_menu("Open Recent", true, I::Scene)) {
            for (const auto& sc : project_.scenes()) {
                if (ctx.menu_item(sc, "", nullptr, true, I::Scene)) { const fs::path p = project_.absolute(sc); guarded_([this, p] { open_scene(p); }); }
            }
            ctx.end_menu();
        }
        ctx.menu_separator();
        if (ctx.menu_item("Save", "Ctrl S", nullptr, true, I::Save)) save_all_();
        if (ctx.menu_item("Save Scene As...", "Shift Ctrl S", nullptr, true, I::Save)) save_scene_as_dialog_();
        ctx.menu_separator();
        if (ctx.menu_item("New Project...", "", nullptr, true, I::Plus)) new_project_dialog_();
        if (ctx.menu_item("Open Project...", "", nullptr, true, I::Folder)) open_project_dialog_();
        if (ctx.begin_menu("Recent Projects", true, I::Folder)) {
            for (const auto& r : Project::recent_projects()) {
                if (ctx.menu_item(r, "", nullptr, true, I::Folder)) { const fs::path p = r; guarded_([this, p] { switch_project_ = p; }); }
            }
            ctx.end_menu();
        }
        ctx.menu_separator();
        if (ctx.menu_item("Package Project (.caml)...", "", nullptr, true, I::Package)) open_package_dialog_();
        ctx.menu_separator();
        if (ctx.menu_item("Quit", "Ctrl Q", nullptr, true, I::X)) { if (request_close()) quit_ = true; }
        ctx.end_menu();
    }

    void draw_edit_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("Edit")) return;
        if (ctx.menu_item("Undo " + current_undo_label_(true), "Ctrl Z", nullptr, true, I::Undo)) undo();
        if (ctx.menu_item("Redo " + current_undo_label_(false), "Shift Ctrl Z", nullptr, true, I::Redo)) redo();
        ctx.menu_separator();
        const bool ok = !asset_view_() && !playing();
        if (ctx.menu_item("Duplicate", "Shift D", nullptr, ok, I::Duplicate)) { duplicate_selected(); pending_modal_kind_ = ModalKind::Grab; }
        if (ctx.menu_item("Delete", "X", nullptr, ok, I::Trash)) delete_selected();
        ctx.menu_separator();
        if (ctx.menu_item("Rename Active Item", "F2", nullptr, ok && doc_.primary() != 0)) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
        if (ctx.menu_item("Preferences...", "", nullptr, true, I::Gear)) pending_modal_ = "Controls";
        ctx.end_menu();
    }

    void draw_render_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("Render")) return;
        if (ctx.menu_item("Render Image", "F12", nullptr, true, I::Render)) render_image_();
        if (ctx.menu_item("Restart Renderer", "", nullptr, true, I::Restart)) restart_ = true;
        ctx.menu_separator();
        if (ctx.menu_item("Render Settings", "", nullptr, true, I::Render)) prop_tab_ = PropTab::Render;
        if (ctx.menu_item("World Settings", "", nullptr, true, I::World)) prop_tab_ = PropTab::World;
        ctx.end_menu();
    }

    void draw_window_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("Window")) return;
        bool t = show_toolbar_, n = show_sidebar_, bt = show_bottom_;
        if (ctx.menu_item("Toolbar", "T", &t)) show_toolbar_ = !show_toolbar_;
        if (ctx.menu_item("Sidebar", "N", &n)) show_sidebar_ = !show_sidebar_;
        if (ctx.menu_item("Asset Browser / Console", "", &bt)) show_bottom_ = !show_bottom_;
        if (ctx.menu_item("Toggle Maximize Area", "Ctrl Space", nullptr, true, I::Zoom)) maximized_ = !maximized_;
        ctx.menu_separator();
        if (ctx.menu_item("Layout", "", nullptr, true, I::Asset)) set_tab(Tab::Layout);
        if (ctx.menu_item("Modeling", "", nullptr, true, I::EditMode)) set_tab(Tab::Modeling);
        if (ctx.menu_item("Shading", "", nullptr, true, I::ShadeMaterial)) set_tab(Tab::Shading);
        ctx.menu_separator();
        if (ctx.menu_item("Reset Layout", "", nullptr, true, I::Restart)) {
            right_w_ = 340; bottom_h_ = 190; outliner_h_ = 260; show_toolbar_ = show_bottom_ = true; show_sidebar_ = false; maximized_ = false;
        }
        ctx.end_menu();
    }

    // --- Unity play controls ---

    void toggle_pause_() {
        if (!play_scene_) return;
        play_scene_->set_simulating(!play_scene_->is_simulating());
        step_countdown_ = 0;
    }

    /** @brief One simulated frame of a paused game (starts play paused if not playing). */
    void step_simulation_() {
        if (!play_scene_) { play(); pause_after_start_ = true; return; }
        play_scene_->set_simulating(true);
        step_countdown_ = 2;   // simulate the next tick, then pause again (see post_late_update_)
    }

    /** @brief F12: writes the current full render (low-res scene image) to <project>/renders/. */
    void render_image_() {
        try {
            const fs::path dir = project_.root() / "renders";
            fs::create_directories(dir);
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            const fs::path p = dir / ("render_" + std::to_string(stamp / 1000000) + ".png");
            engine_.save_screenshot(p.string(), true);
            log_info("Rendered " + p.filename().string() + " (renders/)");
        } catch (const std::exception& e) {
            log_error(std::string("Render failed: ") + e.what());
        }
    }
