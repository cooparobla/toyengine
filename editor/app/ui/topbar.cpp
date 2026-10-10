#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

void EditorApp::draw_topbar_(imm::Context& ctx, const imm::Box& b) {
    using I = imm::Icon;
    ctx.fill(b, et_.chrome.topbar_bg);
    const char* menus[] = {"File", "Edit", "Render", "Build", "Window", "Help"};
    float menus_w = 8;
    for (const char* m : menus) menus_w += ctx.text_width(m) + ctx.style.padding * 3;
    // The toyengine logo (Blender's logo slot): a button with the app menu (About, ...).
    {
        const imm::Box lb{b.x + 4, b.y + 3, b.h - 6, b.h - 6};
        bool hov = false, held = false;
        if (ctx.invisible_button("app_logo", lb, &hov, &held)) ctx.open_popup("app_menu", glm::vec2(lb.x, lb.bottom() + 2));
        if (hov || ctx.is_popup_open("app_menu")) ctx.fill_rounded(lb, ctx.style.button_hover, 5);
        draw_logo_(ctx, lb.shrink(2));
        ctx.tooltip(std::string(core::kEngineName) + " " + core::kVersionString + "\nAbout, controls and quit");
        if (ctx.begin_popup("app_menu", 210)) {
            if (ctx.menu_item("About toyengine...", "", nullptr, true, I::Info)) pending_modal_ = "About";
            if (ctx.menu_item("Controls...", "", nullptr, true, I::Keyboard)) pending_modal_ = "Controls";
            ctx.menu_separator();
            if (ctx.menu_item("Quit", "Cmd Q")) { if (request_close()) quit_ = true; }
            ctx.end_popup();
        }
    }
    const imm::Box mb{b.x + b.h, b.y, menus_w, b.h};
    ctx.begin_menubar(mb);
    draw_file_menu_(ctx);
    draw_edit_menu_(ctx);
    draw_render_menu_(ctx);
    draw_build_menu_(ctx);
    draw_window_menu_(ctx);
    if (ctx.begin_menu("Help")) {
        if (ctx.menu_item("Controls...", "", nullptr, true, I::Keyboard)) pending_modal_ = "Controls";
        if (ctx.menu_item("About toyengine...", "", nullptr, true, I::Info)) pending_modal_ = "About";
        ctx.end_menu();
    }
    ctx.end_menubar();

    // The open asset: type, name, unsaved marker, and the mesh being edited from a scene.
    {
        const auto& info = asset_type_info_(active_type_);
        std::string label = std::string(info.singular) + "  " + active_asset_label_() + (open_asset_dirty_() ? "  *" : "");
        if (edit_object_ && mesh_.open()) label += "   \u25b8   editing " + project_.relative(mesh_.path) + (mesh_.dirty() ? " *" : "");
        const float w = ctx.text_width(label) + b.h + 18;
        const imm::Box ab{mb.right() + 12, b.y + 4, w, b.h - 8};
        ctx.fill_rounded(ab, ctx.style.field);
        ctx.icon(info.icon, {ab.x + 5, ab.y + 2, ab.h - 4, ab.h - 4}, ctx.style.object_active);
        ctx.text_in({ab.x + ab.h + 6, ab.y, ab.w - ab.h - 6, ab.h}, label, ctx.style.text, 0.0f);
        ctx.tooltip(std::string("Open asset\n") + (active_path_.empty() ? std::string("not saved yet") : project_.relative(active_path_)) +
                    " -- pick another in the Asset panel");
    }

    // Unity play controls, centred -- scenes only (play runs the open scene).
    const float s = b.h - 6;
    if (active_type_ != AssetType::Scene) return draw_topbar_project_(ctx, b);
    const float cx = b.x + b.w * 0.5f - s * 1.5f;
    const bool paused = playing() && play_scene_ && !play_scene_->is_simulating() && step_countdown_ == 0;
    ctx.fill_rounded({cx - 2, b.y + 2, s * 3 + 4, s + 2}, et_.chrome.play_group_bg);
    if (ctx.icon_button("tb_play", playing() ? I::Stop : I::Play, playing() ? "Stop\nLeave play mode (F5 / Esc)" : "Play\nRun the scene in the game simulation (F5)",
                        playing(), s, imm::Context::kLeft, imm::Box{cx, b.y + 3, s, s})) {
        playing() ? stop() : play();
    }
    if (ctx.icon_button("tb_pause", I::Pause, "Pause\nFreeze the running simulation", paused, s, 0, imm::Box{cx + s, b.y + 3, s, s})) toggle_pause_();
    if (ctx.icon_button("tb_step", I::Step, "Step\nAdvance the paused simulation by one frame", false, s, imm::Context::kRight,
                        imm::Box{cx + s * 2, b.y + 3, s, s})) {
        step_simulation_();
    }

    draw_topbar_project_(ctx, b);
}

void EditorApp::draw_topbar_project_(imm::Context& ctx, const imm::Box& b) {
    using I = imm::Icon;
    // Project name at the right.
    const std::string proj = project_.name();
    const float sw = ctx.text_width(proj) + 40;
    const imm::Box sb{b.right() - sw - 8, b.y + 4, sw, b.h - 8};
    ctx.icon(I::Folder, {sb.x + 5, sb.y + 2, sb.h - 4, sb.h - 4}, ctx.style.text_dim);
    ctx.text_in({sb.x + sb.h + 4, sb.y, sb.w - sb.h - 4, sb.h}, proj, ctx.style.text_dim, 0.0f);
    ctx.tooltip("Project\n" + project_.root().string());
}

void EditorApp::draw_file_menu_(imm::Context& ctx) {
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

void EditorApp::draw_edit_menu_(imm::Context& ctx) {
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
    draw_theme_menu_(ctx);
    if (ctx.menu_item("Project Settings...", "", nullptr, true, I::Gear)) open_project_settings_("General");
    ctx.tooltip("Project Settings\nThe project's default render, output and physics settings (assets/config.yaml)");
    if (ctx.menu_item("Preferences...", "", nullptr, true, I::Gear)) open_preferences();
    ctx.tooltip("Preferences\nYour editor settings: theme, interface scale, navigation, snapping, keymap");
    ctx.end_menu();
}

void EditorApp::draw_theme_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.begin_menu("Theme", true, I::Palette)) return;
    for (const auto& t : imm::list_themes(editor_themes_dir())) {
        bool on = t.id == theme_id_;
        if (ctx.menu_item(t.name, "", &on)) set_theme(t.id);
        ctx.tooltip(t.name + "\n" + t.path.filename().string() + " -- edits to the file apply live");
    }
    ctx.menu_separator();
    if (ctx.menu_item("Reload Theme", "", nullptr, true, I::Restart)) load_theme_(theme_id_);
    ctx.tooltip("Reload Theme\nRe-read " + (editor_themes_dir() / (theme_id_ + ".yaml")).string());
    if (ctx.menu_item("Export Full Theme", "", nullptr, true, I::File)) export_theme_();
    ctx.tooltip("Export Full Theme\nWrites every role of the current theme to editor/themes/" + theme_id_ + "_full.yaml, a starting point for a new theme");
    ctx.end_menu();
}

void EditorApp::export_theme_() {
    imm::Theme t = theme_;
    // Spell out the editor roles too, so the file lists everything that can be themed.
    visit_editor_theme(et_, [&](const char* section, const char* role, const glm::vec4& c) { t.sections[section][role] = c; });
    const fs::path out = editor_themes_dir() / (theme_id_ + "_full.yaml");
    t.name += " (full)";
    std::ofstream f(out);
    f << "# Exported from \"" << theme_.name << "\" -- every themable role. Rename `name:` and edit.\n" << imm::theme_to_yaml(t);
    if (f) log_info("Theme exported: " + out.string());
    else log_error("Could not write " + out.string());
}

void EditorApp::draw_render_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.begin_menu("Render")) return;
    if (ctx.menu_item("Render Image", "F12", nullptr, true, I::Render)) render_image_();
    if (ctx.menu_item("Rebuild Renderer", "", nullptr, true, I::Restart)) rebuild_renderer_();
    ctx.tooltip("Rebuild Renderer\nRecreates the render pipeline in place from the current settings; the window stays open");
    if (ctx.menu_item("Restart Editor Engine...", "", nullptr, true, I::Restart)) restart_ = true;
    ctx.tooltip("Restart Editor Engine\nRecreates the whole engine and window (open documents are kept). "
                "Only needed for settings baked in at start-up, such as Texel AA");
    ctx.menu_separator();
    if (ctx.menu_item("Project Settings...", "", nullptr, true, I::Gear)) open_project_settings_("General");
    if (ctx.menu_item("Scene Render Settings", "", nullptr, true, I::Render)) prop_tab_ = PropTab::Render;
    if (ctx.menu_item("World Settings", "", nullptr, true, I::World)) prop_tab_ = PropTab::World;
    ctx.end_menu();
}

void EditorApp::draw_window_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.begin_menu("Window")) return;
    bool t = show_toolbar_, n = show_sidebar_, bt = show_bottom_;
    if (ctx.menu_item("Toolbar", "T", &t)) show_toolbar_ = !show_toolbar_;
    if (ctx.menu_item("Sidebar", "N", &n)) show_sidebar_ = !show_sidebar_;
    if (ctx.menu_item("Console", "", &bt)) show_bottom_ = !show_bottom_;
    if (ctx.menu_item("Toggle Maximize Area", "Ctrl Space", nullptr, true, I::Zoom)) maximized_ = !maximized_;
    ctx.menu_separator();

    if (ctx.menu_item("Reset Layout", "", nullptr, true, I::Restart)) {
        left_w_ = 260; right_w_ = 330; bottom_h_ = 130; outliner_h_ = 260; show_toolbar_ = show_bottom_ = true; show_sidebar_ = false; maximized_ = false;
    }
    ctx.end_menu();
}

void EditorApp::toggle_pause_() {
    if (!play_scene_) return;
    play_scene_->set_simulating(!play_scene_->is_simulating());
    if (play_scene_->is_simulating()) engine_.audio().resume_all();
    else engine_.audio().pause_all();
    step_countdown_ = 0;
}

void EditorApp::step_simulation_() {
    if (!play_scene_) { play(); pause_after_start_ = true; return; }
    play_scene_->set_simulating(true);
    step_countdown_ = 2;   // simulate the next tick, then pause again (see post_late_update_)
}

void EditorApp::render_image_() {
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

std::vector<std::pair<std::string, std::string>> EditorApp::about_rows_() {
#if defined(__APPLE__)
    std::string platform = "macOS";
#elif defined(_WIN32)
    std::string platform = "Windows";
#else
    std::string platform = "Linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    platform += " (arm64)";
#elif defined(__x86_64__) || defined(_M_X64)
    platform += " (x86_64)";
#endif
#if defined(__clang__)
    const std::string compiler = std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    const std::string compiler = "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    const std::string compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    const std::string compiler = "unknown";
#endif
#ifdef NDEBUG
    const char* build = "optimized";
#else
    const char* build = "debug";
#endif
    return {
        {"Build", std::string(__DATE__) + " " + __TIME__ + " (" + build + ")"},
        {"Compiler", compiler},
        {"Platform", platform},
        {"Graphics", "Vulkan " + engine_.device().api_version_string() + " on " + engine_.device().gpu_name()},
        {"Libraries", "libcoopa, gfxcoopa, uicoopa, physxcoopa, sfxcoopa, mapcoopa"},
        {"Project", project_.root().string()},
    };
}

void EditorApp::draw_about_modal_(imm::Context& ctx) {
    if (!ctx.begin_modal("About", {520, 318})) return;
    const imm::Box logo = ctx.next_box(84, 84);
    draw_logo_(ctx, logo);
    ctx.same_line(16);
    const imm::Box head = ctx.next_box(84);
    const float rh = ctx.style.row_height;
    ctx.text_in({head.x, head.y + 6, head.w, rh}, "toyengine editor", ctx.style.accent, 0.0f);
    ctx.text_in({head.x, head.y + 6 + rh, head.w, rh}, std::string("Version ") + core::kVersionString, ctx.style.text, 0.0f);
    ctx.text_in({head.x, head.y + 6 + rh * 2, head.w, rh}, "Scenes, objects, meshes, materials and textures for toyengine",
                ctx.style.text_dim, 0.0f);
    ctx.separator();
    const auto rows = about_rows_();
    for (const auto& [k, v] : rows) {
        const imm::Box r = ctx.next_box(rh);
        ctx.text_in({r.x, r.y, 86, r.h}, k, ctx.style.text_dim, 0.0f);
        ctx.text_in({r.x + 90, r.y, r.w - 90, r.h}, v, ctx.style.text, 0.0f);
        ctx.tooltip(k + "\n" + v);
    }
    ctx.spacing();
    if (ctx.button("Copy Info", 110, true, imm::Icon::Duplicate) && ctx.input().set_clipboard) {
        std::string text = std::string("toyengine editor ") + core::kVersionString + "\n";
        for (const auto& [k, v] : rows) text += k + ": " + v + "\n";
        ctx.input().set_clipboard(text);
    }
    ctx.tooltip("Copy Info\nPut these details on the clipboard (handy for bug reports)");
    ctx.same_line();
    if (ctx.button("Close", 100)) ctx.close_modal();
    ctx.end_modal();
}

} // namespace editor
} // namespace toy
