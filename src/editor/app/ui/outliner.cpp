#include "editor/app/ui/outliner.h"
#include "editor/app/ui/weather.h"
#include "editor/app/ui/ui_canvas.h"
#include "editor/app/ui/assets.h"

#include "editor/app/editor_app.h"

namespace toy {
namespace editor {
namespace {

/** @brief Appends every descendant id of `n`, depth-first in document order. */
void collect_children_(const Node& n, std::vector<ObjectId>& out) {
    if (!n.contains("children") || !n.at("children").is_sequence()) return;
    for (const auto& c : n.at("children").as_seq()) {
        if (const ObjectId id = SceneDocument::id_of(c)) out.push_back(id);
        collect_children_(c, out);
    }
}

}  // namespace

void OutlinerPanel::draw_outliner_(imm::Context& ctx, const imm::Box& area) {
    using I = imm::Icon;
    app_.outliner_hovered_ = ctx.is_hovered(area);
    app_.eye_rects_.clear();
    app_.hierarchy_hovered_ = app_.outliner_hovered_;
    const imm::Box hb = app_.area_header_(ctx, area);
    ctx.icon(app_.doc_.is_object_asset() ? I::Object : I::Scene, {hb.x + 6, hb.y + 4, hb.h - 8, hb.h - 8}, ctx.style.text_dim);
    // The area's title, then the filter in what's left of the header.
    const char* title = "Hierarchy";
    const float title_w = ctx.text_width(title) + 10;
    ctx.text_in({hb.x + hb.h + 2, hb.y, title_w, hb.h}, title, ctx.style.text, 0.0f);
    const float sx = hb.x + hb.h + 2 + title_w + 4;
    const imm::Box search{sx, hb.y + 4, std::max(40.0f, hb.right() - hb.h - 6 - sx), hb.h - 8};
    app_.search_box_in_(ctx, "outliner_filter", search, app_.outliner_filter_, "Filter");
    if (ctx.icon_button("outliner_new", I::Plus, "Add Object\nShift A in the viewport (object assets too)", false,
                        hb.h - 6, imm::Context::kAll, imm::Box{hb.right() - hb.h + 2, hb.y + 3, hb.h - 6, hb.h - 6})) {
        app_.open_add_menu_ = true;   // declared by the viewport (draw_viewport_popups_)
    }
    // The button is up before a row is drawn: any drag across the eye (or monitor) column is
    // over, and the one undo entry its rows merged into is closed. Ending it here rather than
    // after the rows is what keeps a stale column from catching a later press that began
    // somewhere else entirely.
    if (drag_column_ != ToggleColumn::None && !ctx.mouse_down()) {
        drag_column_ = ToggleColumn::None;
        drag_painted_.clear();
        app_.doc_.end_merge();
    }
    const imm::Box body{area.x, hb.bottom(), area.w, area.h - hb.h};
    ctx.begin_region("outliner", body, true);
    // An object asset's file IS its object: no scene row above it.
    const bool object_file = app_.doc_.is_object_asset();
    imm::TreeNodeResult root{};
    if (!object_file) {
        root = ctx.tree_node(ctx.get_id("scene_root"), app_.doc_.scene_name(), false, false, true, nullptr, I::Scene);
        ctx.tooltip(app_.doc_.scene_name() + "\nThe scene file's `scene:` block -- click for its settings (Properties > Scene)");
        if (root.clicked) { app_.doc_.clear_selection(); app_.prop_tab_ = PropTab::Scene; }
        if (root.right_clicked && !app_.playing()) { app_.context_target_ = 0; ctx.open_popup("outliner_ctx"); }
    } else {
        root.open = true;
    }
    if (root.open) {
        if (app_.outliner_filter_.empty()) draw_outliner_list_(ctx, app_.doc_.root_objects());
        else {
            std::string f = app_.outliner_filter_;
            std::transform(f.begin(), f.end(), f.begin(), ::tolower);
            app_.doc_.for_each_object([&](const Node& o, int) {
                std::string n = get_string(o, "name");
                std::string ln = n;
                std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
                if (ln.find(f) == std::string::npos) return;
                const ObjectId id = SceneDocument::id_of(o);
                if (outliner_row_skippable_(ctx, id, true)) ctx.next_box(ctx.style.row_height);
                else draw_outliner_row_(ctx, id, true);
            });
        }
        if (!object_file) ctx.tree_pop();
    }
    // Objects systems created at runtime (weather effects...): locked, after the file's own.
    if (!object_file && app_.outliner_filter_.empty() && app_.active_type_ == AssetType::Scene) app_.weather_panel_->draw_outliner_runtime_(ctx);
    // Drop on empty space: un-parent to the root.
    if (!app_.playing()) {
        const float y = ctx.cursor().y;
        if (auto dropped = ctx.drop_target("object", imm::Box{body.x, y, body.w, std::max(16.0f, body.bottom() - y)})) {
            app_.apply_(app_.doc_.reparent(std::stoll(*dropped), 0));
        }
        if (ctx.open_context_popup_in("outliner_ctx", body)) app_.context_target_ = 0;
    }
    if (ctx.begin_popup("outliner_ctx", 240)) {
        draw_object_context_menu_(ctx, app_.context_target_);
        ctx.end_popup();
    }
    ctx.end_region();
}

void OutlinerPanel::draw_outliner_list_(imm::Context& ctx, const Node& list) {
    if (!list.is_sequence()) return;
    std::vector<ObjectId> ids;
    for (const auto& o : list.as_seq()) ids.push_back(SceneDocument::id_of(o));
    for (ObjectId id : ids) {
        if (outliner_row_skippable_(ctx, id, false)) ctx.next_box(ctx.style.row_height);
        else draw_outliner_row_(ctx, id, false);
    }
}

bool OutlinerPanel::outliner_row_skippable_(imm::Context& ctx, ObjectId id, bool flat) {
    if (app_.rename_id_ == id || !ctx.next_row_clipped(ctx.style.row_height)) return false;
    if (flat) return true;   // filter results are flat rows: never open
    ctx.push_id(static_cast<int64_t>(id));
    const bool open = ctx.tree_is_open(ctx.get_id("row"));
    ctx.pop_id();
    return !open;
}

void OutlinerPanel::draw_outliner_row_(imm::Context& ctx, ObjectId id, bool flat) {
    using I = imm::Icon;
    const Node* o = app_.doc_.find(id);
    if (!o) return;
    const std::string name = get_string(*o, "name", "Object");
    const bool instance = SceneDocument::is_instance_node(*o);
    const bool inherited = o->contains(kInheritedKey);
    const Node& shown = app_.instances_panel_->effective_(*o);   // inside an instance: the asset merged with the overrides
    const bool overridden = (instance || inherited) && app_.instances_panel_->has_overrides_(id);
    const bool active = get_bool(shown, "active", true);
    const bool isolated = app_.isolated_.count(id) > 0;   // hidden by Edit / Sculpt Mode isolation
    const bool hidden = app_.hidden_.count(id) > 0 || isolated;
    std::vector<std::string> comps;
    if (shown.contains("components")) {
        for (const auto& c : shown.at("components").as_seq()) comps.push_back(component_type(c));
    }
    const bool has_children = !flat && o->contains("children") && o->at("children").is_sequence() && o->at("children").size() > 0;
    const bool leaf = flat || (!has_children && comps.empty());
    ctx.push_id(static_cast<int64_t>(id));

    if (app_.rename_id_ == id) {
        imm::Box row = ctx.next_box(ctx.style.row_height);
        std::string n = name;
        if (app_.rename_frames_++ == 0) ctx.begin_text_edit("rename", n);
        if (ctx.input_text_box("rename", {row.x + 20, row.y, row.w - 20, row.h}, &n) && !n.empty()) {
            app_.apply_(app_.doc_.set_object_key(id, "name", Node(n), "Rename"));
        }
        if (!ctx.wants_keyboard() && app_.rename_frames_ > 1) app_.rename_id_ = 0;
        ctx.pop_id();
        return;
    }

    auto [icon, tint] = app_.object_icon_(shown, ctx.style);
    if (instance) { icon = I::Link; tint = ctx.style.accent; }
    glm::vec4 text_col = ctx.style.text;
    if (inherited && !overridden) text_col = ctx.style.text_dim;   // straight from the object asset
    if (!active || hidden) { text_col = ctx.style.text_disabled; tint = imm::with_alpha(tint, 0.45f); }
    const bool selected = app_.doc_.is_selected(id);
    const bool is_active = app_.doc_.primary() == id;
    // Row label tinted like Blender: the active object's name brighter.
    const glm::vec4 label_col = is_active ? app_.et_.outliner.active_label : text_col;
    auto r = ctx.tree_node(ctx.get_id("row"), name, leaf, selected, false, &label_col, icon, 46.0f, is_active, &tint);
    if (r.clicked) {
        const bool add = has(ctx.input().mods, Mods::Shift) || has(ctx.input().mods, imm::Context::command_mod());
        app_.doc_.select(id, add);
    }
    if (overridden) ctx.fill({r.rect.x, r.rect.y + 2, 2, r.rect.h - 4}, ctx.style.accent);   // has overrides
    if (r.double_clicked && !app_.playing() && !inherited) { app_.rename_id_ = id; app_.rename_frames_ = 0; }
    if (r.right_clicked) { app_.context_target_ = id; if (!app_.doc_.is_selected(id)) app_.doc_.select(id); ctx.open_popup("outliner_row_ctx"); }
    if (!app_.playing()) {
        if (!inherited) ctx.drag_source("object", std::to_string(id), name);   // the asset places its parts
        if (auto dropped = ctx.drop_target("object", r.rect)) {
            const ObjectId src = std::stoll(*dropped);
            if (src != id) app_.apply_(app_.doc_.reparent(src, id));
        }
        if (app_.active_type_ == AssetType::UI) {
            // The UI designer's palette and UI assets drop onto a row: added as its child.
            if (auto a = ctx.drop_target("asset", r.rect); a && app_.assets_panel_->asset_type_of_(*a) == AssetType::UI) app_.ui_canvas_panel_->place_ui_asset(*a, id);
        }
    }
    // Row toggles: eye (hide in viewport) and monitor (enabled in the game).
    const float s = ctx.style.row_height - 2;
    const imm::Box eye{r.rect.right() - s * 2 - 6, r.rect.y + 1, s, s};
    const imm::Box mon{r.rect.right() - s - 4, r.rect.y + 1, s, s};
    app_.eye_rects_[id] = eye;
    draw_row_toggles_(ctx, id, eye, mon, hidden, isolated, active);
    if (ctx.begin_popup("outliner_row_ctx", 240)) {
        draw_object_context_menu_(ctx, app_.context_target_);
        ctx.end_popup();
    }
    if (instance) ctx.tooltip(name + "\nInstance of " + get_string(*o, "prefab", get_string(*o, "inherit_from")) +
                              " -- edits here are saved as overrides" + (overridden ? " (has overrides)" : ""));
    else if (inherited) ctx.tooltip(name + "\nFrom the object asset -- edits here are saved as overrides on this instance" +
                                    (overridden ? " (has overrides)" : ""));
    if (r.open) {
        if (o->contains("children")) draw_outliner_list_(ctx, o->at("children"));
        for (size_t i = 0; i < comps.size(); ++i) {
            ctx.push_id(static_cast<int64_t>(i));
            const glm::vec4 dim = ctx.style.text_dim;
            auto cr = ctx.tree_node(ctx.get_id("comp"), comps[i], true, false, false, &dim, app_.icon_for_component_(comps[i]));
            if (cr.clicked) {
                app_.doc_.select(id);
                const ComponentSchema* sc = find_schema(comps[i]);
                app_.prop_tab_ = comps[i] == "Transform" ? PropTab::Object : comps[i] == "MeshRenderer" ? PropTab::Material
                          : (sc && sc->category == "Physics") ? PropTab::Physics : PropTab::Components;
            }
            ctx.tooltip(comps[i] + "\nComponent -- click to edit it in the Properties editor");
            ctx.pop_id();
        }
        ctx.tree_pop();
    }
    ctx.pop_id();
}

void OutlinerPanel::draw_row_toggles_(imm::Context& ctx, ObjectId id, const imm::Box& eye, const imm::Box& mon,
                                     bool hidden, bool isolated, bool active) {
    using I = imm::Icon;
    const bool shift = has(ctx.input().mods, Mods::Shift);
    // A press claims the column and sets this row; every frame after, the row the pointer is
    // over takes the same state. The pointer is over at most one row, so one test serves both.
    auto press_in = [&](const imm::Box& b) { return ctx.mouse_pressed() && ctx.is_hovered(b); };
    auto painting = [&](const imm::Box& b, ToggleColumn col) {
        return drag_column_ == col && ctx.mouse_down() && !drag_painted_.count(id) &&
               b.contains(ctx.input().mouse) && ctx.current_clip().contains(ctx.input().mouse);
    };
    auto begin_drag = [&](ToggleColumn col, bool value) {
        drag_column_ = col;
        drag_value_ = value;
        drag_children_ = shift;   // the whole gesture follows the modifier held at press
        drag_painted_.clear();
    };
    if (!isolated) {
        if (press_in(eye)) begin_drag(ToggleColumn::Eye, !hidden);
        if (painting(eye, ToggleColumn::Eye)) {
            set_rows_hidden_(row_subtree_(id, drag_children_), drag_value_);
            drag_painted_.insert(id);
        }
    }
    if (!app_.playing()) {
        if (press_in(mon)) begin_drag(ToggleColumn::Monitor, !active);
        if (painting(mon, ToggleColumn::Monitor)) {
            set_rows_active_(row_subtree_(id, drag_children_), drag_value_);
            drag_painted_.insert(id);
        }
    }
    // The buttons are drawn for their hover, tooltip and keyboard activation; the pointer was
    // handled above, so a click's release -- which is what they report -- is already spent.
    const bool by_mouse = ctx.mouse_pressed() || ctx.mouse_released();
    if (ctx.icon_button("eye", hidden ? I::EyeClosed : I::Eye,
                        isolated ? "Hidden by Isolation\nEdit / Sculpt Mode is showing only the edited object (header toggle)"
                                 : "Hide in Viewport\nShift includes its children; drag down the column to copy. "
                                   "H hides, Alt H reveals (editor only, not saved)",
                        false, eye.h, imm::Context::kAll, eye) && !isolated && !by_mouse) {
        set_rows_hidden_(row_subtree_(id, shift), !hidden);
    }
    if (ctx.icon_button("mon", active ? I::Monitor : I::MonitorOff,
                        "Enabled in Game\nThe object's `active` flag, saved with the scene. Shift includes its "
                        "children; drag down the column to copy",
                        false, mon.h, imm::Context::kAll, mon) && !app_.playing() && !by_mouse) {
        set_rows_active_(row_subtree_(id, shift), !active);
        app_.doc_.end_merge();
    }
}

std::vector<ObjectId> OutlinerPanel::row_subtree_(ObjectId id, bool with_children) const {
    std::vector<ObjectId> out{id};
    const Node* o = with_children ? app_.doc_.find(id) : nullptr;
    if (o) collect_children_(*o, out);
    return out;
}

void OutlinerPanel::set_rows_hidden_(const std::vector<ObjectId>& ids, bool hide) {
    std::vector<ObjectId> mine;
    for (ObjectId id : ids) if (!app_.isolated_.count(id)) mine.push_back(id);   // isolation owns the rest
    if (hide) { app_.hide_(mine); return; }
    for (ObjectId id : mine) {
        app_.hidden_.erase(id);
        const Node* n = app_.doc_.find(id);
        // Back to what the document says: revealing an object does not enable a disabled one.
        if (auto* live = app_.sync_.live(id)) {
            live->set_active(n ? get_bool(app_.instances_panel_->effective_(*n), "active", true) : true);
        }
    }
}

void OutlinerPanel::set_rows_active_(const std::vector<ObjectId>& ids, bool value) {
    app_.apply_(app_.doc_.edit(value ? "Enable Object" : "Disable Object", [&](Node&) -> Change {
        ObjectId top = 0;
        for (ObjectId id : ids) {
            if (Node* o = app_.doc_.find(id)) { (*o)["active"] = Node(value); if (!top) top = id; }
        }
        // Rebuilding the topmost row rebuilds its subtree with it (SceneSync::rebuild_object_).
        return top ? Change{ChangeScope::Object, top} : Change{};
    }, "outliner_active"));
}

void OutlinerPanel::draw_object_context_menu_(imm::Context& ctx, ObjectId target) {
    using I = imm::Icon;
    const bool ok = !app_.playing();
    app_.instances_panel_->draw_instance_menu_items_(ctx, target);   // Open Object Asset, Revert / Apply overrides
    if (ctx.begin_menu("Add Child", ok && target != 0, I::Plus)) {
        if (ctx.menu_item("Empty", "", nullptr, true, I::Empty)) app_.create_empty(target);
        for (const auto& p : primitive_names()) if (ctx.menu_item(p, "", nullptr, true, I::Cube)) app_.create_primitive(p, target);
        if (ctx.menu_item("Point Light", "", nullptr, true, I::PointLight)) app_.create_with_component("PointLight", "Point Light", target);
        ctx.end_menu();
    }
    if (ctx.begin_menu("Add", ok, I::Plus)) {
        if (ctx.menu_item("Empty", "", nullptr, true, I::Empty)) app_.create_empty();
        for (const auto& p : primitive_names()) if (ctx.menu_item(p, "", nullptr, true, I::Cube)) app_.create_primitive(p);
        ctx.end_menu();
    }
    if (ctx.begin_menu("Object Asset", ok && !app_.doc_.is_object_asset(), I::Object)) {
        for (const auto& rel : app_.assets_panel_->list_assets_(AssetType::Object)) {
            if (ctx.menu_item(fs::path(rel).stem().string(), "", nullptr, true, I::Link)) app_.assets_panel_->place_object_asset(rel);
        }
        if (app_.assets_panel_->list_assets_(AssetType::Object).empty()) ctx.label_dim("No object assets yet");
        ctx.end_menu();
    }
    if (ctx.menu_item("Create Object Asset", "", nullptr, ok && target != 0 && !app_.doc_.is_object_asset(), I::Object)) {
        app_.doc_.select(target);
        app_.assets_panel_->create_object_asset_from_selection();
    }
    ctx.tooltip("Create Object Asset\nSave this object as objects/<name>.yaml and replace it with an instance");
    ctx.menu_separator();
    if (ctx.menu_item("Rename", "F2", nullptr, ok && target != 0 && !app_.doc_.is_inherited(target))) { app_.rename_id_ = target; app_.rename_frames_ = 0; }
    if (ctx.menu_item("Duplicate", "Shift D", nullptr, ok && target != 0, I::Duplicate)) app_.duplicate_selected();
    if (ctx.menu_item("Delete", "X", nullptr, ok && target != 0, I::Trash)) app_.delete_selected();
    if (ctx.menu_item("Clear Parent", "Alt P", nullptr, ok && target != 0)) app_.clear_parent_keep_transform_();
    ctx.menu_separator();
    if (ctx.menu_item("Hide", "H", nullptr, target != 0, I::EyeClosed)) app_.hide_(app_.doc_.selection());
    if (ctx.menu_item("Show All", "Alt H", nullptr, true, I::Eye)) app_.unhide_all_();
    if (ctx.menu_item("Frame", "F / .", nullptr, target != 0, I::Zoom)) app_.frame_selected();
}

} // namespace editor
} // namespace toy
