/**
 * @file hub_app.h
 * @brief toyengine Hub: a small Unity-Hub-style launcher for toyengine projects, drawn with the
 *        editor's immediate-mode UI, themes and file dialog so it looks like the editor.
 *
 * Pages: Projects (the list: open in the editor, reveal, re-pin / link the engine, remove;
 * New / Add with their options), Engine (this checkout, CLI install, macOS app), Settings (the
 * hub's own theme, ~/.toyengine/settings.yaml, blender_dark by default). Every project operation
 * runs tools/toyhub as a Task (editor/core/process.h) whose output streams into the log drawer at
 * the bottom; the hub itself never builds, clones or writes a project. Building is the editor's
 * job (Build > Refresh) -- the hub only builds a project the first time it is opened, since the
 * editor it opens is part of that build.
 *
 * Embeds a toy::core::Engine only for its window, device and UI rendering -- no scene is
 * loaded; the UI is an overlay scene, as in the editor.
 */

#ifndef TOYENGINE_HUB_HUB_APP_H
#define TOYENGINE_HUB_HUB_APP_H

#include "hub_core.h"

#include "../editor/app/editor_theme.h"
#include "../editor/core/naming.h"
#include "../editor/ui/log_view.h"
#include "../editor/ui/logo.h"
#include "../editor/viewport/editor_camera.h"

#include <toyengine/core/branding.h>
#include <toyengine/core/engine.h>

#include <uicoopa/immediate/imm_canvas.h>
#include <uicoopa/immediate/imm_file_dialog.h>
#include <uicoopa/immediate/imm_theme.h>

#include <root_directory.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace toy::hub {

namespace imm = coopa::ui::imm;
using editor::EditorCamera;
using editor::EditorTheme;
using imm::FileDialog;
using editor::draw_logo;
using editor::editor_theme_from;
using editor::editor_themes_dir;

class HubApp {
public:
    enum class Page { Projects = 0, Engine, Settings };

    explicit HubApp(core::Engine& engine) : engine_(engine) {
        ui_scene_ = std::make_unique<coopa::scene::Scene>("HubUI");
        canvas_ = coopa::ui::build_immediate_canvas(*ui_scene_, "HubUI", 100000);
        canvas_->on_draw = [this](imm::Context& ctx) { draw_(ctx); };
        default_font_ = std::string(PROJ_DIR) + "/uicoopa/assets/fonts/Inter-Regular.ttf";
        current_font_ = default_font_;
        canvas_->context().text.set_font(coopa::ui::UIResourceCache::instance().font_for_path(default_font_));
        // The hub's own theme setting (~/.toyengine/settings.yaml), independent of the editor's.
        const Node settings = load_settings();
        std::string id = default_hub_theme();
        if (settings.contains("theme") && settings.at("theme").is_string()) id = settings.at("theme").get_value<std::string>();
        if (!load_theme_(id)) load_theme_(default_hub_theme());

        // The Engine only renders (overlay included) with an active scene: an empty backdrop
        // with a camera, which the full-window UI covers.
        coopa::scene::Scene& backdrop = engine_.set_scene(coopa::scene::Scene("HubBackdrop"));
        camera_.create(backdrop);
        engine_.set_overlay_scene(ui_scene_.get());
        engine_.set_edit_mode(true);
        core::FrameHooks hooks;
        hooks.pre_render = [this](float) {
            if (auto* c = canvas_component_()) c->scaler.scale_factor = std::max(1.0f, engine_.display_scale());
            camera_.make_main();
        };
        hooks.post_late_update = [this](float) { poll_task_(); };
        engine_.set_frame_hooks(std::move(hooks));

        // The picker: this session's folders under Recent (projects are added from anywhere).
        file_dialog_.on_folder_used = [this](const fs::path& dir, FileDialog::Mode) {
            auto& r = file_dialog_.recent_dirs;
            r.erase(std::remove(r.begin(), r.end(), dir), r.end());
            r.insert(r.begin(), dir);
            if (r.size() > 8) r.resize(8);
        };

        engine_head_ = command_output("git -C " + shell_quote(ROOT_DIR) + " log -1 --format='%h  %s'");
        refresh();
    }

    ~HubApp() {
        task_.wait();
        engine_.set_frame_hooks({});
        engine_.set_overlay_scene(nullptr);
        engine_.wait_idle();
    }

    HubApp(const HubApp&) = delete;
    HubApp& operator=(const HubApp&) = delete;

    /**
     * @brief The hub's window and renderer: the engine's own assets/config.yaml (the defaults
     *        everything else runs with -- the bare AppConfig{} code defaults carry the
     *        pixel-art toggles), with the hub's window and the 3D work it never shows
     *        switched off.
     */
    static core::AppConfig engine_config(bool visible) {
        core::AppConfig cfg = core::AppConfig::load((fs::path(ROOT_DIR) / "assets" / "config.yaml").string());
        cfg.window.title = "toyengine Hub";
        cfg.window.width = 1100;
        cfg.window.height = 680;
        cfg.window.visible = visible;
        cfg.window.vsync = visible;
        cfg.render.screen_ui_enabled = true;
        cfg.output.save_on_exit = false;
        auto& r = cfg.render;
        r.resolution_mode = "fill";
        r.upscale_mode = "fit";
        r.ssao_enabled = r.ssr_enabled = r.refraction_enabled = r.sdf_enabled = r.shadows_enabled = false;
        r.volumetrics_enabled = r.bloom_enabled = r.auto_exposure_enabled = false;
        r.aa_mode = "off";
        return cfg;
    }
    static core::EngineOptions engine_options() {
        core::EngineOptions o;
        o.project_root = ROOT_DIR;
        o.load_default_scene = false;
        o.edit_mode = true;
        o.escape_quits = false;
        return o;
    }

    // --- queries / scripted control (main loop, tests) ---

    /** @brief Where row `i`'s Open and "..." buttons were drawn last frame (canvas points). */
    imm::Box row_open_box(size_t i) const { return i < row_open_boxes_.size() ? row_open_boxes_[i] : imm::Box{}; }
    imm::Box row_menu_box(size_t i) const { return i < row_menu_boxes_.size() ? row_menu_boxes_[i] : imm::Box{}; }
    /** @brief The project whose "..." menu was drawn last frame (-1: none). */
    int menu_shown_for() const { return menu_shown_for_; }
    /** @brief The modal drawn last frame ("" if none): "New Project", "Add Project", "Remove Project". */
    const std::string& modal_shown() const { return modal_shown_; }
    const std::string& status() const { return status_; }
    const editor::LogView& log_view() const { return log_view_; }

    bool quit_requested() const { return quit_; }
    const std::vector<HubProject>& projects() const { return projects_; }
    Task& task() { return task_; }
    Page page() const { return page_; }
    void set_page(Page p) { page_ = p; }
    imm::Context& ui() { return canvas_->context(); }

    /** @brief Re-reads the project list (~/.toyengine/projects.yaml). */
    void refresh() {
        projects_ = load_projects();
        if (selected_ >= static_cast<int>(projects_.size())) selected_ = -1;
    }

    /** @brief Runs `toyhub <args>` in the log drawer; `then` runs on the UI thread on success. */
    bool run_toyhub(const std::string& title, const std::vector<std::string>& args, std::function<void()> then = {}) {
        if (!task_.start(title, toyhub_command(args))) return false;
        log_view_.follow();
        task_gen_ = task_.generation();
        on_success_ = std::move(then);
        log_open_ = true;
        return true;
    }

    /**
     * @brief Opens the project in its editor. A project that has never been built is built first
     *        (toyhub open, output in the log); after that, building is the editor's Build > Refresh.
     */
    void open_project(const HubProject& p) {
        if (p.built()) {
            launch_detached(shell_quote((p.root / "editor.sh").string()), p.root / ".toyeditor" / "editor.log");
            status_ = "Opened " + p.name + " in the editor";
            return;
        }
        run_toyhub("First build of " + p.name + " (then opening the editor)", {"open", p.root.string()});
    }

    /** @brief The New / Add options: where, and which engine the project builds against. */
    struct ProjectForm {
        bool adding = false;              ///< Add (an existing folder) vs New
        std::string name = "my_game";     ///< New: the folder (= project) name, always snake_case
        std::string location;             ///< New: the parent folder
        std::string folder;               ///< Add: the folder
        int engine_mode = 0;              ///< 0: pinned clone at a commit, 1: linked to a local checkout
        std::string ref;                  ///< Pinned: empty = this checkout's HEAD (must be pushed)
        std::string engine_path = ROOT_DIR;   ///< Linked: any toyengine checkout
        bool setup_engine = true;         ///< Fetch / link .libs/toyengine now (setup.sh)

        fs::path target() const { return adding ? fs::path(folder) : fs::path(location) / editor::snake_case(name); }
    };
    ProjectForm& project_form() { return form_; }

    /** @brief The New project button: the options dialog for a new folder. */
    void open_new_project_dialog() {
        form_.adding = false;
        if (form_.location.empty()) form_.location = default_location_().string();
        pending_modal_ = "New Project";
    }

    /** @brief The Add button: pick a folder, then list it (it has a project) or set it up (options). */
    void open_add_project_dialog() {
        file_dialog_.open(ui(), FileDialog::Mode::PickFolder, "Add Project Folder", default_location_(), {},
                          [this](const fs::path& p) { add_folder(p); });
    }

    /**
     * @brief A folder picked for Add: one that already holds a project (a .toy with a target) is
     *        listed as it is; any other is set up as a new project in place, after the options.
     */
    void add_folder(const fs::path& dir) {
        if (is_engine_checkout(dir)) { status_ = "The engine checkout is not a project"; return; }
        if (has_target_(dir)) {   // already a project: list it as it is
            run_toyhub("Add " + dir.filename().string(), {"add", dir.string()}, [this, dir] { select_(dir); });
            return;
        }
        form_.adding = true;
        form_.folder = normalized(dir);
        pending_modal_ = "Add Project";
    }

    /** @brief Runs `toyhub new` / `toyhub add` with the form's options. */
    void submit_project_form() {
        const fs::path dir = form_.target();
        std::vector<std::string> args{form_.adding ? "add" : "new", dir.string()};
        if (form_.engine_mode == 1) { args.push_back("--link"); args.push_back(form_.engine_path); }
        else if (!form_.ref.empty()) { args.push_back("--ref"); args.push_back(form_.ref); }
        if (!form_.setup_engine) args.push_back("--no-setup");
        run_toyhub((form_.adding ? "Add " : "Create ") + dir.filename().string(), args, [this, dir] { select_(dir); });
    }

    /** @brief The "Remove Project..." confirmation for `p`. */
    void open_remove_dialog(const HubProject& p) {
        remove_target_ = p;
        pending_modal_ = "Remove Project";
    }

    /** @brief Moves the project's folder to the Trash and stops listing it (toyhub delete). */
    void delete_project(const HubProject& p) {
        run_toyhub("Delete " + p.name, {"delete", p.root.string(), "--yes"});
    }

private:
    // =================================================================================
    // Frame
    // =================================================================================

    void draw_(imm::Context& ctx) {
        row_open_boxes_.assign(projects_.size(), imm::Box{});
        row_menu_boxes_.assign(projects_.size(), imm::Box{});
        menu_shown_for_ = -1;
        modal_shown_.clear();
        const glm::vec2 sz = ctx.canvas_size();
        ctx.fill({0, 0, sz.x, sz.y}, ctx.style.panel_bg);   // fields (style.field) read against it
        const float side_w = 208;
        const float log_h = log_open_ ? std::min(260.0f, sz.y * 0.4f) : 26.0f;
        draw_sidebar_(ctx, {0, 0, side_w, sz.y});
        const imm::Box main{side_w, 0, std::max(0.0f, sz.x - side_w), std::max(0.0f, sz.y - log_h)};
        switch (page_) {
            case Page::Projects: draw_projects_(ctx, main); break;
            case Page::Engine:   draw_engine_(ctx, main); break;
            case Page::Settings: draw_settings_(ctx, main); break;
        }
        draw_log_(ctx, {side_w, sz.y - log_h, std::max(0.0f, sz.x - side_w), log_h});
        if (!pending_modal_.empty()) { ctx.open_modal(pending_modal_); pending_modal_.clear(); reopen_after_picker_.clear(); }
        draw_modals_(ctx);
        const bool picker_was_open = file_dialog_.is_open();
        file_dialog_.draw(ctx);
        // A picker opened from a form and cancelled: back to the form.
        if (picker_was_open && !file_dialog_.is_open() && !reopen_after_picker_.empty()) {
            pending_modal_ = reopen_after_picker_;
            reopen_after_picker_.clear();
        }
    }

    /** @brief Fires the success callback once the current toyhub task finishes. */
    void poll_task_() {
        if (task_gen_ == 0 || task_.running() || task_.generation() != task_gen_) return;
        task_gen_ = 0;
        if (task_.succeeded()) {
            status_ = task_.title() + " -- done";
            if (auto fn = std::move(on_success_)) { on_success_ = {}; fn(); }
            refresh();
        } else {
            status_ = task_.title() + " -- failed (exit " + std::to_string(task_.exit_code()) + ")";
            on_success_ = {};
            log_open_ = true;
        }
    }

    // =================================================================================
    // Sidebar
    // =================================================================================

    void draw_sidebar_(imm::Context& ctx, const imm::Box& b) {
        ctx.fill(b, et_.chrome.topbar_bg);
        draw_logo(ctx, {b.x + 16, b.y + 16, 34, 34});
        ctx.draw_text({b.x + 58, b.y + 18}, core::kEngineName, ctx.style.text, ctx.style.font_size * 1.35f);
        ctx.draw_text({b.x + 58, b.y + 38}, std::string("Hub  ") + core::kVersionString, ctx.style.text_dim);

        struct Item { Page page; imm::Icon icon; const char* label; };
        const Item items[] = {
            {Page::Projects, imm::Icon::Folder, "Projects"},
            {Page::Engine, imm::Icon::Gear, "Engine"},
            {Page::Settings, imm::Icon::Palette, "Settings"},
        };
        float y = b.y + 72;
        for (const auto& it : items) {
            const imm::Box r{b.x + 8, y, b.w - 16, 30};
            bool hov = false;
            if (ctx.invisible_button(std::string("nav_") + it.label, r, &hov)) page_ = it.page;
            const bool on = page_ == it.page;
            if (on) ctx.fill_rounded(r, ctx.style.selection, ctx.style.rounding);
            else if (hov) ctx.fill_rounded(r, ctx.style.row_hover, ctx.style.rounding);
            ctx.icon(it.icon, {r.x + 8, r.y + 7, 16, 16}, on ? ctx.style.text : ctx.style.text_dim);
            ctx.text_in({r.x + 32, r.y, r.w - 32, r.h}, it.label, on ? ctx.style.text : ctx.style.text_dim, 0.0f);
            y += 34;
        }
        ctx.text_in({b.x + 12, b.bottom() - 26, b.w - 24, 20}, fs::path(ROOT_DIR).filename().string() + "  (engine checkout)",
                    ctx.style.text_disabled, 0.0f);
    }

    // =================================================================================
    // Projects page
    // =================================================================================

    /** @brief A flat button drawn at `b` (primary: accent-filled). */
    bool button_at_(imm::Context& ctx, const std::string& id, const imm::Box& b, const std::string& label, bool primary = false,
                    bool enabled = true, imm::Icon icon = imm::Icon::None) {
        bool hov = false, held = false;
        const bool clicked = ctx.invisible_button(id, b, &hov, &held) && enabled;
        glm::vec4 bg = primary ? ctx.style.accent : ctx.style.button;
        if (!enabled) bg.a *= 0.4f;
        else if (held) bg = primary ? ctx.style.button_active * 0.85f : ctx.style.button_active;
        else if (hov) bg = primary ? glm::min(ctx.style.accent * 1.15f, glm::vec4(1.0f)) : ctx.style.button_hover;
        ctx.fill_rounded(b, bg, ctx.style.rounding);
        const glm::vec4 fg = enabled ? ctx.style.text : ctx.style.text_disabled;
        if (icon != imm::Icon::None) {
            const float tw = ctx.text_width(label), iw = 16, total = iw + (label.empty() ? 0 : 6 + tw);
            const float x0 = b.x + (b.w - total) * 0.5f;
            ctx.icon(icon, {x0, b.y + (b.h - iw) * 0.5f, iw, iw}, fg);
            if (!label.empty()) ctx.text_in({x0 + iw + 6, b.y, tw + 4, b.h}, label, fg, 0.0f);
        } else {
            ctx.text_in(b, label, fg, 0.0f, true);
        }
        return clicked;
    }

    void draw_page_header_(imm::Context& ctx, const imm::Box& b, const std::string& title) {
        ctx.draw_text({b.x + 24, b.y + 22}, title, ctx.style.text, ctx.style.font_size * 1.6f);
    }

    void draw_projects_(imm::Context& ctx, const imm::Box& b) {
        draw_page_header_(ctx, b, "Projects");
        // Header actions, right-aligned: [search] [Add] [New project].
        float x = b.right() - 24;
        const float hy = b.y + 18, hh = 28;
        x -= 130;
        if (button_at_(ctx, "new_project", {x, hy, 130, hh}, "New project", true, !task_.running(), imm::Icon::Plus)) {
            open_new_project_dialog();
        }
        x -= 8 + 80;
        if (button_at_(ctx, "add_project", {x, hy, 80, hh}, "Add...", false, true, imm::Icon::Folder)) {
            open_add_project_dialog();
        }
        x -= 8 + 220;
        ctx.input_text_box("search", {x, hy, 220, hh}, &search_, "Search projects");

        // Column headers.
        const imm::Box list{b.x + 16, b.y + 64, b.w - 32, b.h - 64 - 8};
        const float col_engine = list.w - 440, col_mod = list.w - 290;
        ctx.text_in({list.x + 56, list.y, 200, 22}, "NAME", ctx.style.text_dim, 0.0f);
        ctx.text_in({list.x + col_engine, list.y, 120, 22}, "ENGINE", ctx.style.text_dim, 0.0f);
        ctx.text_in({list.x + col_mod, list.y, 120, 22}, "MODIFIED", ctx.style.text_dim, 0.0f);
        ctx.fill({list.x, list.y + 24, list.w, 1}, ctx.style.separator);

        const imm::Box rows{list.x, list.y + 26, list.w, list.h - 26};
        ctx.begin_region("project_rows", rows, true);
        int shown = 0;
        for (size_t i = 0; i < projects_.size(); ++i) {
            const HubProject& p = projects_[i];
            if (!matches_search_(p)) continue;
            ++shown;
            ctx.push_id(static_cast<int64_t>(i));
            draw_project_row_(ctx, p, static_cast<int>(i), col_engine, col_mod);
            ctx.pop_id();
        }
        if (shown == 0) {
            const imm::Box e = ctx.next_box(80);
            ctx.text_in(e, projects_.empty() ? "No projects yet -- create one with New project, or Add an existing folder."
                                             : "No project matches the search.",
                        ctx.style.text_dim, 0.0f, true);
        }
        ctx.end_region();
    }

    bool matches_search_(const HubProject& p) const {
        if (search_.empty()) return true;
        auto lower = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        const std::string q = lower(search_);
        return lower(p.name).find(q) != std::string::npos || lower(p.root.string()).find(q) != std::string::npos;
    }

    void draw_project_row_(imm::Context& ctx, const HubProject& p, int index, float col_engine, float col_mod) {
        const imm::Box r = ctx.next_box(54);
        // Background first, but the row's own click target LAST: imm gives a press to the first
        // widget declared under it, so declaring the row before Open / "..." would swallow theirs.
        const bool sel = selected_ == index;
        if (sel) ctx.fill_rounded(r, ctx.style.selection_dim, ctx.style.rounding);
        else if (ctx.is_hovered(r)) ctx.fill_rounded(r, ctx.style.row_hover, ctx.style.rounding);

        draw_logo(ctx, {r.x + 10, r.y + 11, 32, 32});
        ctx.push_clip({r.x + 56, r.y, std::max(0.0f, col_engine - 56 - 12), r.h});
        ctx.draw_text({r.x + 56, r.y + 10}, p.name, ctx.style.text, ctx.style.font_size * 1.1f);
        ctx.draw_text({r.x + 56, r.y + 30}, p.root.string(), ctx.style.text_dim);
        ctx.pop_clip();

        // Engine badge.
        const std::string eng = p.engine_label();
        const float bw = ctx.text_width(eng) + 16;
        const imm::Box badge{r.x + col_engine, r.y + 16, bw, 22};
        ctx.fill_rounded(badge, p.linked() ? ctx.style.accent * glm::vec4(1, 1, 1, 0.45f) : ctx.style.field, 11);
        ctx.text_in(badge, eng, p.linked() ? ctx.style.text : ctx.style.text_dim, 0.0f, true);
        if (ctx.is_hovered(badge)) {
            ctx.tooltip(p.linked() ? "Linked to " + p.engine_link + "\nBuilds against that working tree, uncommitted edits included."
                                    : "Pinned engine commit " + p.engine_ref + "\n(.libs/toyengine is a clone checked out there)");
        }
        ctx.text_in({r.x + col_mod, r.y, 120, r.h}, relative_time(p.modified), ctx.style.text_dim, 0.0f);

        // Actions: Open (the editor), and the "..." menu.
        const imm::Box ob{r.right() - 120, r.y + 13, 76, 28};
        const imm::Box mb{r.right() - 38, r.y + 13, 28, 28};
        row_open_boxes_[static_cast<size_t>(index)] = ob;
        row_menu_boxes_[static_cast<size_t>(index)] = mb;
        if (button_at_(ctx, "open", ob, "Open", true, !task_.running())) { selected_ = index; open_project(p); }
        if (ctx.icon_button("more", imm::Icon::Dots, "More actions", false, 28, imm::Context::kAll, mb)) {
            selected_ = index;
            ctx.open_popup("project_menu", glm::vec2(r.right() - 260, r.y + 42));
        }
        if (ctx.invisible_button("row", r)) selected_ = index;

        if (selected_ == index && ctx.begin_popup("project_menu", 260)) {
            menu_shown_for_ = index;
            const bool idle = !task_.running();
            const std::string dir = p.root.string();
            if (ctx.menu_item("Open in Editor", "", nullptr, idle, imm::Icon::Play)) open_project(p);
            if (ctx.menu_item("Reveal in Finder", "", nullptr, true, imm::Icon::Folder)) reveal_in_file_browser(p.root);
            ctx.menu_separator();
            if (!p.linked() && ctx.menu_item("Re-pin engine to this checkout's HEAD", "", nullptr, idle, imm::Icon::Restart)) {
                run_toyhub("Re-pin " + p.name, {"upgrade", dir});
            }
            if (ctx.menu_item(p.linked() ? "Link to another toyengine checkout..." : "Link to a local toyengine checkout...", "", nullptr, idle,
                              imm::Icon::Link)) {
                const HubProject target = p;
                file_dialog_.open(ctx, FileDialog::Mode::PickFolder, "Choose a toyengine Checkout", fs::path(ROOT_DIR).parent_path(), {},
                                  [this, target](const fs::path& eng) {
                                      if (!is_toyengine_checkout(eng)) { status_ = eng.string() + " is not a toyengine checkout"; return; }
                                      run_toyhub("Link " + target.name, {"link", target.root.string(), eng.string()});
                                  });
            }
            if (p.linked() && ctx.menu_item("Unlink engine (use the pinned clone)", "", nullptr, idle, imm::Icon::Link)) {
                run_toyhub("Unlink " + p.name, {"unlink", dir});
            }
            ctx.menu_separator();
            if (ctx.menu_item("Remove Project...", "", nullptr, idle, imm::Icon::Trash)) open_remove_dialog(p);
            ctx.end_popup();
        }
    }

    fs::path default_location_() const {
        if (!projects_.empty()) return projects_.front().root.parent_path();
        const char* home = std::getenv("HOME");
        return fs::path(home ? home : ".");
    }

    // =================================================================================
    // Engine / Settings pages
    // =================================================================================

    void draw_engine_(imm::Context& ctx, const imm::Box& b) {
        draw_page_header_(ctx, b, "Engine");
        ctx.begin_region("engine_page", {b.x + 16, b.y + 60, std::min(760.0f, b.w - 32), b.h - 68}, true);
        auto row = [&](const char* label, const std::string& value) {
            const imm::Box r = ctx.property_row(label);
            ctx.text_in(r, value, ctx.style.text, 0.0f);
        };
        row("Checkout", ROOT_DIR);
        row("Version", core::kVersionString);
        row("HEAD", engine_head_.empty() ? std::string("(not a git checkout)") : engine_head_);
        row("Libraries", std::string(PROJ_DIR));
        ctx.spacing(8);
        ctx.paragraph("New projects pin this checkout's HEAD commit and clone git@github.com:cooparobla/toyengine.git into "
                      "<project>/.libs/toyengine, so the commit has to be pushed. A linked project builds against this "
                      "checkout directly instead, uncommitted engine and libs/ edits included.");
        ctx.spacing(8);
        const bool idle = !task_.running();
        if (ctx.button("Install toyhub command", 220, idle, imm::Icon::Console)) run_toyhub("Install toyhub", {"install"});
        ctx.label_dim("Links tools/toyhub into ~/.local/bin, for `toyhub new|list|open|build ...` in a terminal.");
#if defined(__APPLE__)
        ctx.spacing(6);
        if (ctx.button("Create toyengine Hub.app", 220, idle, imm::Icon::Package)) run_toyhub("Create Hub app", {"app"});
        ctx.label_dim("Writes ~/Applications/toyengine Hub.app, a launcher for this build of the hub (Dock / Launchpad / Spotlight).");
#endif
        ctx.spacing(6);
        if (ctx.button("Reveal checkout", 220, true, imm::Icon::Folder)) reveal_in_file_browser(ROOT_DIR);
        ctx.end_region();
    }

    void draw_settings_(imm::Context& ctx, const imm::Box& b) {
        draw_page_header_(ctx, b, "Settings");
        ctx.begin_region("settings_page", {b.x + 16, b.y + 60, std::min(560.0f, b.w - 32), b.h - 68}, true);
        std::vector<std::string> ids;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(editor_themes_dir(), ec)) {
            if (e.path().extension() == ".yaml") ids.push_back(e.path().stem().string());
        }
        std::sort(ids.begin(), ids.end());
        int idx = static_cast<int>(std::find(ids.begin(), ids.end(), theme_id_) - ids.begin());
        if (ctx.combo("Theme", &idx, ids) && idx >= 0 && idx < static_cast<int>(ids.size()) && load_theme_(ids[idx])) {
            Node settings = load_settings();
            settings["theme"] = Node(ids[idx]);
            save_settings(settings);
        }
        ctx.label_dim("The hub's own setting (~/.toyengine/settings.yaml); the editor keeps its own.");
        ctx.end_region();
    }

    // =================================================================================
    // Log drawer and modals
    // =================================================================================

    void draw_log_(imm::Context& ctx, const imm::Box& b) {
        ctx.fill(b, et_.chrome.statusbar_bg);
        const imm::Box bar{b.x, b.y, b.w, 26};
        bool hov = false;
        if (ctx.invisible_button("log_toggle", {bar.x, bar.y, bar.w - 90, bar.h}, &hov)) log_open_ = !log_open_;
        ctx.arrow({bar.x + 8, bar.y + 7, 12, 12}, log_open_, ctx.style.text_dim);
        std::string head = status_;
        if (task_.running()) {
            static const char* spin[] = {"|", "/", "-", "\\"};
            head = std::string(spin[(engine_.frame_count() / 8) % 4]) + "  " + task_.title() + " ...";
        }
        if (head.empty()) head = "Ready";
        const glm::vec4 col = (!task_.running() && task_.exit_code() > 0) ? ctx.style.error : ctx.style.text_dim;
        ctx.text_in({bar.x + 26, bar.y, bar.w - 120, bar.h}, head, col, 0.0f);
        if (!task_.running() && task_.line_count() > 0 &&
            button_at_(ctx, "log_clear", {bar.right() - 80, bar.y + 3, 70, 20}, "Clear")) {
            task_.clear();
            status_.clear();
        }
        if (!log_open_) return;
        // Sticks to the newest line while output streams in, unless scrolled up to read.
        log_view_.draw(ctx, {b.x + 4, b.y + 26, b.w - 8, b.h - 30}, task_.lines());
    }

    void draw_modals_(imm::Context& ctx) {
        for (const char* title : {"New Project", "Add Project"}) {
            if (!ctx.begin_modal(title, {600, 0})) continue;
            modal_shown_ = title;
            draw_project_form_(ctx, title);
            ctx.end_modal();
        }
        if (ctx.begin_modal("Remove Project", {520, 0})) {
            modal_shown_ = "Remove Project";
            ctx.paragraph("Remove \"" + remove_target_.name + "\"?");
            ctx.label_dim(remove_target_.root.string());
            ctx.spacing(4);
#if defined(__APPLE__)
            ctx.paragraph("Move to Trash deletes the project folder (you can put it back from the Trash). Remove from List "
                          "only stops showing it here; its files stay.");
#else
            ctx.paragraph("Move to Trash deletes the project folder. Remove from List only stops showing it here; its files stay.");
#endif
            if (remove_target_.linked()) ctx.label_dim("Its linked engine checkout (" + remove_target_.engine_link + ") is not touched.");
            ctx.spacing(6);
            if (ctx.button("Move to Trash", 130, !task_.running(), imm::Icon::Trash)) {
                ctx.close_modal();
                delete_project(remove_target_);
            }
            ctx.same_line();
            if (ctx.button("Remove from List", 140)) {
                ctx.close_modal();
                forget_project(remove_target_.root);
                status_ = "Removed " + remove_target_.name + " from the list (files untouched)";
                refresh();
            }
            ctx.same_line();
            if (ctx.button("Cancel", 90) || escape_(ctx)) ctx.close_modal();
            ctx.end_modal();
        }
    }

    void draw_project_form_(imm::Context& ctx, const char* title) {
        ProjectForm& f = form_;
        auto browse = [&](const char* picker_title, fs::path start, std::function<void(const fs::path&)> set) {
            ctx.close_modal();
            const std::string reopen = title;
            file_dialog_.open(ctx, FileDialog::Mode::PickFolder, picker_title, start, {},
                              [this, set, reopen](const fs::path& p) { set(p); pending_modal_ = reopen; });
            // A cancelled picker reopens the form too, on the frame it closes.
            reopen_after_picker_ = reopen;
        };
        if (f.adding) {
            ctx.paragraph("This folder has no project file yet. It will be set up as a toyengine project in place: a "
                          ".toy file, src/, the build / run / editor scripts and the engine in .libs/ are added, and "
                          "nothing already in the folder is overwritten.");
            const imm::Box r = ctx.property_row("Folder");
            ctx.text_in(r, f.folder, ctx.style.text, 0.0f);
            ctx.label_dim("Project name: " + fs::path(f.folder).filename().string() + "  (always the folder's name)");
        } else {
            ctx.paragraph("Creates the folder <location>/<name> from the project template. The folder name is the "
                          "project's name, in snake_case (My Game becomes my_game).");
            // Project names are snake_case, like everything in assets/: "My Game" becomes my_game.
            if (ctx.input_text("Name", &f.name)) f.name = editor::snake_case(f.name);
            ctx.input_text("Location", &f.location);
            if (ctx.button("Browse...", 100)) browse("Choose Location", fs::path(f.location), [this](const fs::path& p) { form_.location = p.string(); });
        }
        ctx.spacing(6);
        static const std::vector<std::string> modes = {"Pinned to a commit (cloned into .libs/)", "Linked to a local toyengine checkout"};
        ctx.combo("Engine", &f.engine_mode, modes);
        bool engine_ok = true;
        if (f.engine_mode == 0) {
            ctx.input_text("Commit / tag", &f.ref);
            ctx.label_dim("Empty: this checkout's HEAD. Clones git@github.com:cooparobla/toyengine.git, so it must be pushed.");
        } else {
            ctx.input_text("Checkout", &f.engine_path);
            if (ctx.button("Browse...##engine", 100)) {
                browse("Choose a toyengine Checkout", fs::path(f.engine_path).parent_path(), [this](const fs::path& p) { form_.engine_path = p.string(); });
            }
            engine_ok = is_toyengine_checkout(f.engine_path);
            if (!engine_ok) ctx.label("Not a toyengine checkout (no cmake/ToyProject.cmake or toyengine/core/engine.h).", &ctx.style.warning);
            else ctx.label_dim(".libs/toyengine becomes a symlink to it: its uncommitted engine and libs/ edits build into the project.");
        }
        ctx.spacing(4);
        ctx.property_bool(f.engine_mode == 0 ? "Clone the engine now" : "Link the engine now", &f.setup_engine);
        ctx.spacing(6);
        std::error_code ec;
        bool target_ok = true;
        if (!f.adding) {
            const fs::path t = f.target();
            const bool exists = !f.name.empty() && fs::exists(t, ec) && !fs::is_empty(t, ec);
            if (exists) ctx.label("Exists and is not empty: " + t.string() + " (use Add instead)", &ctx.style.warning);
            target_ok = !f.name.empty() && !f.location.empty() && !exists;
        }
        if (ctx.button(f.adding ? "Add" : "Create", 100, target_ok && engine_ok && !task_.running())) {
            ctx.close_modal();
            submit_project_form();
        }
        ctx.same_line();
        if (ctx.button("Cancel", 100) || escape_(ctx)) ctx.close_modal();
    }

    /** @brief Escape cancels a dialog -- unless a text field has the keyboard (Escape ends that edit). */
    static bool escape_(imm::Context& ctx) {
        return !ctx.wants_keyboard() && ctx.key_pressed(coopa::input::Key::Escape);
    }

    bool has_target_(const fs::path& dir) const {
        const fs::path toy = Project::find_project_file(dir);
        return !toy.empty() && !get_string(Project::load_toy(toy), "target").empty();
    }

    void select_(const fs::path& dir) {
        refresh();
        for (size_t i = 0; i < projects_.size(); ++i) if (projects_[i].root == fs::path(normalized(dir))) selected_ = static_cast<int>(i);
    }

    // =================================================================================
    // Theme
    // =================================================================================

    bool load_theme_(const std::string& id) {
        try {
            theme_ = imm::load_theme(editor_themes_dir() / (id + ".yaml"));
        } catch (const std::exception& e) {
            std::cerr << "[hub] theme: " << e.what() << "\n";
            return false;
        }
        theme_id_ = id;
        et_ = editor_theme_from(theme_);
        imm::Context& ctx = canvas_->context();
        ctx.style = theme_.style;
        const std::string font = theme_.font.empty() ? default_font_ : theme_.font;
        if (font != current_font_) {
            if (auto* f = coopa::ui::UIResourceCache::instance().font_for_path(font)) { ctx.text.set_font(f); current_font_ = font; }
        }
        return true;
    }

    coopa::ui::CanvasComponent* canvas_component_() {
        coopa::scene::SceneObject* o = canvas_->owner ? canvas_->owner->parent() : nullptr;
        return o ? o->get_component<coopa::ui::CanvasComponent>() : nullptr;
    }

    core::Engine& engine_;
    std::unique_ptr<coopa::scene::Scene> ui_scene_;
    coopa::ui::ImmediateCanvas* canvas_ = nullptr;
    imm::Theme theme_;
    EditorTheme et_;
    std::string theme_id_, default_font_, current_font_;
    FileDialog file_dialog_;
    EditorCamera camera_;

    Page page_ = Page::Projects;
    std::vector<HubProject> projects_;
    int selected_ = -1;
    std::string search_;
    ProjectForm form_;
    HubProject remove_target_;
    std::string pending_modal_, reopen_after_picker_;
    std::vector<imm::Box> row_open_boxes_, row_menu_boxes_;
    int menu_shown_for_ = -1;
    std::string modal_shown_;
    std::string engine_head_;

    Task task_;
    int task_gen_ = 0;
    std::function<void()> on_success_;
    std::string status_;
    bool log_open_ = false;
    editor::LogView log_view_;
    bool quit_ = false;
};

} // namespace toy::hub

#endif // TOYENGINE_HUB_HUB_APP_H
