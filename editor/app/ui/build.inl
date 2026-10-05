// editor/app/ui/build.inl -- included inside EditorApp's class body.
//
// Build > Refresh: rebuilds the project's game + editor -- the build this editor is running from
// -- so src/ edits made outside the editor (VS Code, ...) take effect. A blocking modal streams
// the build log; compiler errors are listed (click one to open it in VS Code / the default app)
// and also go to the Console. A build that changed the editor binary offers to relaunch it,
// since new C++ only loads in a new process.

public:
    /**
     * @brief The build command: `cmake --build` on the directory this editor binary was built in,
     *        for its own target and its game's (a game project: build/<target>_editor + <target>;
     *        toyengine itself: toyengine_editor + toyengine, not the tests).
     */
    static std::string default_build_command() {
        const fs::path exe = current_executable();
        const fs::path dir = exe.parent_path();
        std::string targets = " --target " + shell_quote(exe.filename().string());
#ifdef TOY_GAME_BINARY
        targets += " " + shell_quote(fs::path(TOY_GAME_BINARY).filename().string());
#endif
        const unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
        return tool_path_prefix() + "cmake --build " + shell_quote(dir.string()) + targets + " --parallel " + std::to_string(jobs);
    }

    /** @brief Starts Build > Refresh (`command` overrides the build command: tests). */
    void build_refresh(const std::string& command = {}) {
        if (build_task_.running()) return;
        stop();   // the running game holds the old code
        build_diags_.clear();
        build_relaunch_offered_ = false;
        const fs::path exe = current_executable();
        std::error_code ec;
        build_binary_stamp_ = fs::last_write_time(exe, ec);
        if (command.empty() && !fs::exists(exe.parent_path() / "CMakeCache.txt", ec)) {
            log_error("Build: " + exe.parent_path().string() + " is not a CMake build directory");
            return;
        }
        build_task_.start("Refresh " + project_.name(), command.empty() ? default_build_command() : command);
        build_log_.follow();
        build_gen_ = build_task_.generation();
        pending_modal_ = "Build Project";
        log_info("Build > Refresh: building " + project_.name() + "...");
    }

    const Task& build_task() const { return build_task_; }
    const std::vector<Diagnostic>& build_diagnostics() const { return build_diags_; }
    /** @brief True once the user chose to relaunch after a build (main() restarts the process). */
    bool relaunch_requested() const { return relaunch_; }
    /** @brief The relaunch prompt after a build that changed this editor's binary. */
    bool build_relaunch_offered() const { return build_relaunch_offered_; }
    /** @brief Relaunch now (asks about unsaved changes first, like Quit). */
    void relaunch_after_build() {
        if (!has_unsaved()) { relaunch_ = true; return; }
        confirm_unsaved_([this] { force_quit_ = true; relaunch_ = true; });
    }

private:
    void draw_build_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (ctx.begin_menu("Build")) {
            if (ctx.menu_item("Refresh", "Shift Ctrl B", nullptr, !build_task_.running(), I::Restart)) build_refresh();
            ctx.tooltip("Rebuild this project's game and editor so src/ changes made outside the editor take effect");
            ctx.end_menu();
        }
    }

    /** @brief Once the build finishes: diagnostics to the Console, relaunch offer if needed. */
    void poll_build_() {
        if (build_gen_ == 0 || build_task_.running() || build_task_.generation() != build_gen_) return;
        build_gen_ = 0;
        const auto lines = build_task_.lines();
        std::set<std::string> seen;
        for (const auto& l : lines) {
            if (l.rfind("$ ", 0) == 0) continue;   // the echoed command itself
            auto d = parse_diagnostic(l);
            if (!d || !seen.insert(d->text).second) continue;
            build_diags_.push_back(*d);
        }
        int errors = 0, warnings = 0;
        for (const auto& d : build_diags_) {
            const std::string where = d.file + (d.line > 0 ? ":" + std::to_string(d.line) : "");
            if (d.error) { ++errors; log_error("Build: " + where + ": " + d.message); }
            else { ++warnings; log_warn("Build: " + where + ": " + d.message); }
        }
        if (build_task_.cancelled()) {
            log_warn("Build cancelled");
        } else if (build_task_.succeeded()) {
            std::error_code ec;
            const bool changed = fs::last_write_time(current_executable(), ec) != build_binary_stamp_;
            build_relaunch_offered_ = changed;
            log_info(changed ? "Build succeeded -- relaunch the editor to load the new code"
                             : "Build succeeded -- already up to date");
        } else {
            if (errors == 0) log_error("Build failed (exit " + std::to_string(build_task_.exit_code()) + ") -- see the build log");
            else log_error("Build failed: " + std::to_string(errors) + " error" + (errors == 1 ? "" : "s") +
                           (warnings ? ", " + std::to_string(warnings) + " warning" + (warnings == 1 ? "" : "s") : ""));
        }
    }

    void draw_build_modal_(imm::Context& ctx) {
        const glm::vec2 cs = ctx.canvas_size();
        // Not "Build": that id is the top bar's Build menu, which would then draw inside the modal.
        if (!ctx.begin_modal("Build Project", {std::min(900.0f, cs.x - 60), std::min(600.0f, cs.y - 60)})) return;
        const bool running = build_task_.running();
        const bool ok = !running && build_task_.succeeded();
        const imm::Box R = ctx.content_region();
        const imm::Style& st = ctx.style;

        // Status line.
        std::string head;
        glm::vec4 col = st.text;
        if (running) {
            static const char* spin[] = {"|", "/", "-", "\\"};
            head = std::string(spin[(engine_.frame_count() / 8) % 4]) + "  Building " + project_.name() + " ...";
        } else if (build_task_.cancelled()) {
            head = "Build cancelled";
            col = st.warning;
        } else if (ok) {
            head = build_relaunch_offered_ ? "Build succeeded -- the editor was rebuilt" : "Build succeeded -- already up to date";
            col = glm::vec4(0.45f, 0.85f, 0.45f, 1.0f);
        } else {
            int errors = 0;
            for (const auto& d : build_diags_) errors += d.error;
            head = "Build failed" + (errors ? " -- " + std::to_string(errors) + " error" + (errors == 1 ? "" : "s") : std::string());
            col = st.error;
        }
        const imm::Box hb{R.x, R.y, R.w, st.row_height + 4};
        ctx.text_in(hb, head, col, 0.0f);

        // Diagnostics (after a build), then the log.
        const float bar_h = st.row_height + 6;
        float y = hb.bottom() + 4;
        if (!running && !build_diags_.empty()) {
            const float diag_h = std::min(160.0f, (st.row_height + 1) * static_cast<float>(build_diags_.size()) + 10);
            const imm::Box db{R.x, y, R.w, diag_h};
            const glm::vec4 bg = st.panel_alt;
            ctx.begin_region("build_diags", db, true, &bg);
            for (size_t i = 0; i < build_diags_.size(); ++i) {
                const Diagnostic& d = build_diags_[i];
                const imm::Box r = ctx.next_box(st.row_height);
                ctx.push_id(static_cast<int64_t>(i));
                bool hov = false;
                if (ctx.invisible_button("diag", r, &hov) && !d.file.empty()) open_in_code_editor(d.file, d.line, d.column);
                ctx.pop_id();
                if (hov) ctx.fill_rounded(r, st.row_hover);
                ctx.icon(d.error ? imm::Icon::Error : imm::Icon::Warning, {r.x + 4, r.y + 3, r.h - 6, r.h - 6}, d.error ? st.error : st.warning);
                const std::string where = fs::path(d.file).filename().string() + (d.line > 0 ? ":" + std::to_string(d.line) : "");
                ctx.text_in({r.x + r.h + 4, r.y, 220, r.h}, where, st.text, 0.0f);
                ctx.text_in({r.x + r.h + 228, r.y, r.w - r.h - 232, r.h}, d.message, st.text_dim, 0.0f);
                if (hov) ctx.tooltip(d.text + "\nClick to open it in your code editor");
            }
            ctx.end_region();
            y = db.bottom() + 6;
        }
        const imm::Box lb{R.x, y, R.w, R.bottom() - bar_h - 6 - y};
        build_log_.draw(ctx, lb, build_task_.lines());   // follows the output unless scrolled up

        // Buttons.
        const imm::Box bar{R.x, R.bottom() - bar_h, R.w, bar_h};
        const float bw = 150;
        imm::Box b{bar.right() - bw, bar.y, bw, bar.h};
        if (running) {
            if (build_button_(ctx, "Cancel Build", b)) build_task_.cancel();
        } else {
            if (build_relaunch_offered_) {
                if (build_button_(ctx, "Relaunch Editor", b, true)) { ctx.close_modal(); relaunch_after_build(); }
                b.x -= bw + 8;
                if (build_button_(ctx, "Later", b)) ctx.close_modal();
            } else {
                if (build_button_(ctx, "Close", b, ok)) ctx.close_modal();
                if (!ok) {
                    b.x -= bw + 8;
                    if (build_button_(ctx, "Rebuild", b)) build_refresh();
                }
            }
            b.x -= bw + 8;
            if (build_button_(ctx, "Copy Log", b)) {
                std::string all;
                for (const auto& l : build_task_.lines()) all += l + "\n";
                if (ctx.input().set_clipboard) ctx.input().set_clipboard(all);
                log_info("Build log copied to the clipboard");
            }
        }
        ctx.end_modal();
    }

    /** @brief A button at a fixed box (primary: accent-filled, the default action). */
    static bool build_button_(imm::Context& ctx, const char* label, const imm::Box& b, bool primary = false) {
        bool hov = false, held = false;
        const bool clicked = ctx.invisible_button(label, b, &hov, &held);
        const imm::Style& st = ctx.style;
        glm::vec4 c = primary ? st.accent : st.button;
        if (held) c = primary ? c * 0.85f + glm::vec4(0, 0, 0, 0.15f) : st.button_active;
        else if (hov) c = primary ? glm::mix(c, glm::vec4(1.0f), 0.12f) : st.button_hover;
        ctx.fill_rounded(b, c);
        ctx.text_in(b, label, primary ? glm::vec4(1.0f) : st.text, 0.0f, true);
        return clicked;
    }

    Task build_task_;
    int build_gen_ = 0;
    std::vector<Diagnostic> build_diags_;
    fs::file_time_type build_binary_stamp_{};
    bool build_relaunch_offered_ = false;
    bool relaunch_ = false;
    LogView build_log_;
