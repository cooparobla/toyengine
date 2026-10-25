#include "editor/app/ui/statusbar.h"
#include "editor/app/ui/ui_canvas.h"
#include "editor/app/ui/paint.h"

#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

void StatusBarPanel::draw_statusbar_(imm::Context& ctx, const imm::Box& b) {
    using I = imm::Icon;
    ctx.fill(b, app_.et_.chrome.statusbar_bg);
    // --- hints ---
    std::vector<std::pair<I, std::string>> hints;
    if (app_.modal_.active()) {
        hints = {{I::MouseLeft, "Confirm"}, {I::MouseRight, "Cancel"}, {I::Keyboard, "X Y Z  Axis"}, {I::Keyboard, "Ctrl  Snap"},
                 {I::Keyboard, "Shift  Precision"}};
        if (app_.proportional_live_()) hints.push_back({I::MouseMiddle, "Wheel  Proportional Size"});
    } else if (app_.nav_active_) {
        hints = {{I::MouseMiddle, "Orbit"}, {I::Keyboard, "Shift  Pan"}, {I::Keyboard, "Ctrl  Zoom"}};
    } else if (app_.timeline_hovered_ && app_.bottom_view_ == 1 && app_.anim_clip_.open()) {
        hints = {{I::MouseLeft, "Select / Drag Keys"}, {I::MouseRight, "Key Menu"}, {I::Keyboard, "I  Key"},
                 {I::Keyboard, "X  Delete"}, {I::Keyboard, "Space  Play"}, {I::Keyboard, "Up/Down  Next Key"},
                 {I::MouseMiddle, "Zoom / Pan"}};
    } else if (app_.in_sculpt_mode_()) {
        hints = {{I::MouseLeft, "Sculpt"}, {I::Keyboard, "Ctrl  Invert"}, {I::Keyboard, "Shift  Smooth"}, {I::Keyboard, "F  Radius"},
                 {I::Keyboard, "Shift F  Strength"}, {I::MouseMiddle, "Rotate View"}, {I::Keyboard, "Tab  Object Mode"}};
    } else if (app_.paint_panel_->in_paint_mode_()) {
        if (app_.paint_panel_->in_weight_paint_()) {
            hints = {{I::MouseLeft, "Paint Weight"}, {I::Keyboard, "Ctrl  Subtract"}, {I::Keyboard, "Shift  Blur"},
                     {I::Keyboard, "S  Sample"}, {I::Keyboard, "F  Radius"}, {I::Keyboard, "Shift F  Strength"}, {I::Keyboard, "Tab  Object Mode"}};
        } else {
            hints = {{I::MouseLeft, "Paint"}, {I::Keyboard, "Ctrl  Secondary"}, {I::Keyboard, "Shift  Blur"}, {I::Keyboard, "X  Swap Colors"},
                     {I::Keyboard, "S  Sample"}, {I::Keyboard, "F  Radius"}, {I::Keyboard, "Tab  Object Mode"}};
        }
    } else if (app_.active_type_ == AssetType::UI) {
        if (app_.ui_canvas_panel_->ui_interact_) hints = {{I::MouseLeft, "Use the UI"}, {I::Keyboard, "Esc / Tab  Design"}, {I::MouseMiddle, "Pan"}};
        else if (app_.ui_canvas_panel_->ui_gizmo_.dragging()) hints = {{I::Keyboard, "Shift  Keep Aspect / 15 deg"}, {I::Keyboard, "Alt  From Centre"}, {I::Keyboard, "Ctrl  Snap Off"}};
        else hints = {{I::MouseLeft, "Select / Move"}, {I::Keyboard, "Alt  Cycle"}, {I::Keyboard, "Shift A  Add"},
                      {I::Keyboard, "Arrows  Nudge"}, {I::Keyboard, "Tab  Interact"}, {I::Keyboard, "F / Home  Frame"}, {I::MouseMiddle, "Pan"}};
    } else if (app_.loopcut_.active) {
        hints = {{I::MouseLeft, "Cut and Slide"}, {I::MouseMiddle, "Cuts (wheel)"}, {I::MouseRight, "Cancel"}, {I::Keyboard, "1-9  Cuts"}};
    } else if (app_.mesh_edit_view_()) {
        hints = {{I::MouseLeft, "Select"}, {I::MouseMiddle, "Rotate View"}, {I::MouseRight, "Context Menu"},
                 {I::Keyboard, "G R S  Transform"}, {I::Keyboard, "E  Extrude"}, {I::Keyboard, "Tab  Object Mode"}};
    } else {
        hints = {{I::MouseLeft, "Select"}, {I::MouseMiddle, "Rotate View"}, {I::MouseRight, "Object Context Menu"},
                 {I::Keyboard, "Shift A  Add"}, {I::Keyboard, "Tab  Edit Mode"}};
    }
    float x = b.x + 8;
    for (const auto& [ic, text] : hints) {
        ctx.icon(ic, {x, b.y + 4, b.h - 8, b.h - 8}, ctx.style.text_dim);
        x += b.h - 4;
        const float tw = ctx.text_width(text);
        ctx.text_in({x, b.y, tw + 4, b.h}, text, ctx.style.text_dim, 0.0f);
        x += tw + 16;
    }
    // --- stats, Blender style (placed first, so the report can't run into them) ---
    std::string stats = scene_stats_();
    if (app_.playing()) stats = "PLAYING  |  " + stats;
    stats += std::string("  |  ") + core::kEngineName + " " + core::kVersionString;
    const float sw = ctx.text_width(stats);
    const float stats_x = b.right() - sw - 10;
    ctx.text_in({stats_x, b.y, sw + 6, b.h}, stats, ctx.style.text_dim, 0.0f);
    // --- latest report, clipped to the space left between the hints and the stats ---
    const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - app_.status_time_).count();
    const float room = stats_x - 20 - (x + 10 + b.h);
    if (!app_.status_.empty() && age < 8.0 && room > 40) {
        const I ic = app_.status_level_ == 2 ? I::Error : app_.status_level_ == 1 ? I::Warning : I::Info;
        const glm::vec4 c = app_.status_level_ == 2 ? ctx.style.error : app_.status_level_ == 1 ? ctx.style.warning : ctx.style.text;
        ctx.icon(ic, {x + 10, b.y + 4, b.h - 8, b.h - 8}, app_.status_level_ == 0 ? ctx.style.text_dim : glm::vec4(1));
        std::string msg = app_.status_;
        if (ctx.text_width(msg) > room) {
            while (!msg.empty() && ctx.text_width(msg + "...") > room) msg.pop_back();
            msg += "...";
        }
        ctx.text_in({x + 10 + b.h, b.y, room, b.h}, msg, c, 0.0f);
    }
}

std::string StatusBarPanel::scene_stats_() {
    char buf[200];
    if (app_.paint_panel_->in_brush_mode_() && app_.mesh_.open()) {
        const auto& m = app_.mesh_.mesh;
        std::snprintf(buf, sizeof(buf), "%s  |  Verts %zu  |  Faces %zu  |  Tris %zu", app_.mesh_.name.c_str(), m.positions.size(),
                      m.faces.size(), m.triangle_count());
        return buf;
    }
    if (app_.mesh_edit_view_() && app_.mesh_.open()) {
        const auto& m = app_.mesh_.mesh;
        const auto vs = app_.mesh_.selection.affected_vertices(m);
        const auto fs_ = app_.mesh_.selection.affected_faces(m);
        std::snprintf(buf, sizeof(buf), "%s  |  Verts %zu/%zu  |  Faces %zu/%zu  |  Tris %zu", app_.mesh_.name.c_str(), vs.size(),
                      m.positions.size(), fs_.size(), m.faces.size(), m.triangle_count());
        return buf;
    }
    // The totals only change with the document, the live scene or the selection: recount
    // then, not every frame (the count walks every object's mesh).
    const auto key = std::make_tuple(app_.doc_.undo_revision(), static_cast<uint64_t>(app_.instances_panel_->scene_gen_), app_.doc_.selection().size(),
                                     app_.sync_.live_objects().size());
    if (key == app_.stats_key_ && !app_.stats_cache_.empty()) return app_.stats_cache_;
    size_t verts = 0, faces = 0, tris = 0, objects = 0;
    for (const auto& [id, live] : app_.sync_.live_objects()) {
        ++objects;
        const Node* n = app_.doc_.find(id);
        if (!n || !live || !live->active()) continue;
        if (const EditorApp::CachedMesh* cm = app_.mesh_for_object_(*n)) {
            verts += cm->mesh.positions.size();
            faces += cm->mesh.faces.size();
            tris += cm->mesh.triangle_count();
        }
    }
    std::snprintf(buf, sizeof(buf), "%s  |  Objects %zu/%zu  |  Verts %zu  |  Faces %zu  |  Tris %zu", app_.doc_.scene_name().c_str(),
                  app_.doc_.selection().size(), objects, verts, faces, tris);
    app_.stats_key_ = key;
    app_.stats_cache_ = buf;
    return buf;
}

} // namespace editor
} // namespace toy
