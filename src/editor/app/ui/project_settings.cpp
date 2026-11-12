#include "editor/app/ui/project_settings.h"
#include "editor/app/ui/properties.h"

#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

void ProjectSettingsPanel::open_project_settings_(const std::string& category) {
    app_.project_settings_category_ = category;
    app_.pending_modal_ = "Project Settings";
}

void ProjectSettingsPanel::rebuild_renderer_() {
    app_.apply_config_live();
    app_.engine_.restart_renderer();
    app_.log_info("Renderer rebuilt");
}

std::vector<std::string> ProjectSettingsPanel::project_settings_categories_() {
    std::vector<std::string> cats;
    for (const auto& g : render_settings_groups()) {
        if (std::find(cats.begin(), cats.end(), g.category) == cats.end()) cats.push_back(g.category);
    }
    cats.push_back("Output");
    cats.push_back("Physics");
    if (has_unrecognized_render_keys_()) cats.push_back("Unrecognized");
    return cats;
}

bool ProjectSettingsPanel::has_unrecognized_render_keys_() {
    const auto known = render_settings_keys();
    const std::set<std::string> skip(known.begin(), known.end());
    for (const auto& kv : app_.config_.section("render").as_map()) {
        if (kv.first.is_string() && !skip.count(kv.first.get_value<std::string>())) return true;
    }
    return false;
}

void ProjectSettingsPanel::draw_project_settings_modal_(imm::Context& ctx) {
    using I = imm::Icon;
    const glm::vec2 canvas = ctx.canvas_size();
    const glm::vec2 size{std::min(920.0f, canvas.x - 40.0f), std::min(680.0f, canvas.y - 40.0f)};
    if (!ctx.begin_modal("Project Settings", size)) return;
    if (app_.close_project_settings_) { ctx.close_modal(); app_.close_project_settings_ = false; }   // close_project_settings()
    app_.project_settings_mode_ = true;
    const glm::vec2 top = ctx.cursor();
    const float w = ctx.available_width();
    const float footer_h = ctx.style.row_height + 8;
    const float box_bottom = std::floor((canvas.y - size.y) * 0.5f) + size.y;   // begin_modal() centres the box
    const float body_h = std::max(100.0f, box_bottom - ctx.style.padding - footer_h - top.y);
    const float list_w = 170;
    const imm::Box list{top.x, top.y, list_w, body_h};
    const imm::Box pane{top.x + list_w + 6, top.y, w - list_w - 6, body_h};

    // Left: the categories.
    ctx.begin_region("ps_categories", list, false, &ctx.style.panel_alt);
    for (const auto& c : project_settings_categories_()) {
        if (ctx.selectable(c, c == app_.project_settings_category_)) app_.project_settings_category_ = c;
        app_.test_rects_["project_settings:" + c] = ctx.last_rect();
    }
    ctx.end_region();

    // Right: the search box, then the category's settings (a search spans every category).
    ctx.begin_region("ps_pane", pane, true);
    ctx.label_dim("The project's defaults (assets/config.yaml). Scenes can override them in their Properties tabs.");
    const std::string needle = app_.properties_panel_->settings_search_box_(ctx, "project_settings_filter", app_.project_settings_filter_);
    ctx.spacing(4);
    const std::string& cat = app_.project_settings_category_;
    const InspectorEnv env = app_.inspector_env_();
    if (!needle.empty()) {
        if (!app_.properties_panel_->draw_render_groups_(ctx, needle)) ctx.label_dim("No render setting matches that search.");
    } else if (cat == "Output" || cat == "Physics") {
        for (const auto& g : project_settings_groups()) {
            if ((g.title == "Physics") != (cat == "Physics")) continue;
            const std::string key = project_section_key(g.title);
            if (cat == "Output" && !ctx.collapsing_header(g.title, true)) continue;
            Node& section = app_.config_.section(key);
            ctx.indent(4);
            for (const auto& f : g.fields) app_.draw_setting_row_(ctx, f, section, env, key);
            ctx.unindent(4);
        }
        if (cat == "Output") ctx.label_dim("Window, jobs and debug settings apply the next time the game starts.");
    } else if (cat == "Unrecognized") {
        ctx.label_dim("In config.yaml's render: but not a setting this editor knows.");
        const auto known = render_settings_keys();
        const std::set<std::string> skip(known.begin(), known.end());
        const Node before = app_.config_.node;
        EditResult r = draw_fields(ctx, {}, app_.config_.section("render"), env, true, skip);
        if (r.changed) { app_.config_.commit("Edit render." + r.key, before, r.active ? "cfg:" + r.key : std::string()); app_.apply_config_live(); }
        if (r.finished) app_.config_.undo.end_merge();
    } else {
        app_.properties_panel_->draw_render_groups_(ctx, {}, cat);
    }
    ctx.end_region();

    // Footer.
    ctx.set_cursor_y(top.y + body_h + 6);
    if (ctx.button(app_.config_.dirty() ? "Save config.yaml *" : "Save config.yaml", 160, true, I::Save)) app_.save_config();
    app_.test_rects_["project_settings:save"] = ctx.last_rect();
    ctx.same_line();
    app_.properties_panel_->rebuild_renderer_button_(ctx, 150);
    ctx.same_line();
    if (ctx.button("Close", 100)) ctx.close_modal();
    app_.project_settings_mode_ = false;
    ctx.end_modal();
}

} // namespace editor
} // namespace toy
