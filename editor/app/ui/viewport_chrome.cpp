#include "editor/app/ui/viewport_chrome.h"
#include "editor/app/ui/timeline.h"
#include "editor/app/ui/tile_mode.h"
#include "editor/app/ui/terrain.h"
#include "editor/app/ui/instance_paint.h"
#include "editor/app/ui/sculpt.h"
#include "editor/app/ui/properties.h"
#include "editor/app/ui/paint.h"
#include "editor/app/ui/mesh_tools.h"
#include "editor/app/ui/instances.h"
#include "editor/app/ui/assets.h"

#include "editor/app/editor_app.h"
#include "editor/app/ui/curve_pen.h"
#include "editor/app/ui/preferences.h"
#include "editor/app/ui/weights.h"
#include "editor/app/ui/statusbar.h"

namespace toy {
namespace editor {

float ViewportChromePanel::sidebar_w_() const {
    return app_.show_sidebar_ ? 230.0f : 0.0f;
}

imm::Box ViewportChromePanel::sidebar_rect_() const {
    return {app_.viewport_box_.right() - sidebar_w_(), app_.viewport_box_.y, sidebar_w_(), app_.viewport_box_.h};
}

imm::Box ViewportChromePanel::nav_gizmo_rect_() const {
    return {app_.viewport_box_.right() - sidebar_w_() - 118, app_.viewport_box_.y + 8, 110, 110 + 4 * 30};
}

imm::Box ViewportChromePanel::toolbar_rect_(bool mesh_edit) const {
    // `n` SIZES the panel, so it has to match what draw_toolbar_() actually puts in it. A button
    // added without bumping this draws below the rect and is clipped away -- present, wired, and
    // invisible. Object mode: select, cursor, pen, move, rotate, scale. Pen mode: select, draw,
    // the four anchor modes, show-handles, and the Object Mode exit -- 8 buttons.
    const int n = app_.curve_pen_panel_->in_pen_mode_() ? 8
                : app_.in_sculpt_mode_() ? 5 : app_.paint_panel_->in_paint_mode_() ? 3 : mesh_edit ? 9 : 6;
    return {app_.viewport_box_.x + 6, app_.viewport_box_.y + 6, kToolSize + 6, n * (kToolSize + 2) + 14};
}

bool ViewportChromePanel::preview_only_view_() const {
    return app_.active_type_ == AssetType::Material || app_.active_type_ == AssetType::Texture || app_.active_type_ == AssetType::Audio;
}

bool ViewportChromePanel::over_viewport_chrome_(glm::vec2 p, bool mesh_edit) const {
    if (app_.show_toolbar_ && !preview_only_view_() && toolbar_rect_(mesh_edit).contains(p)) return true;
    const imm::Box g = nav_gizmo_rect_();
    if (glm::distance(p, glm::vec2(g.x + 55, g.y + 55)) < 52.0f) return true;
    if (imm::Box{g.x + 40, g.y + 112, 30, 4 * 30}.contains(p)) return true;
    if (app_.show_sidebar_ && sidebar_rect_().contains(p)) return true;
    if (app_.last_op_rect_.contains(p)) return true;
    return false;
}

void ViewportChromePanel::draw_viewport_header_(imm::Context& ctx, const imm::Box& hb, bool mesh_edit) {
    using I = imm::Icon;
    float x = hb.x + 6;
    const float bh = hb.h - 6;
    if (app_.playing()) ctx.fill_rounded(hb, app_.et_.chrome.play_tint, 6, imm::Context::kTop);   // Unity's play tint
    // Mode dropdown.
    if (app_.active_type_ == AssetType::Scene || app_.active_type_ == AssetType::Object || app_.active_type_ == AssetType::Mesh) {
        const bool sculpting = app_.in_sculpt_mode_();
        const bool terrain = app_.terrain_panel_->in_terrain_mode_();
        const bool tiles = app_.tile_mode_panel_->in_tile_mode_();
        const bool painting_instances = app_.instance_paint_panel_->in_instance_mode_();
        const bool vpaint = app_.paint_panel_->in_paint_mode_() && !app_.paint_panel_->in_weight_paint_(), wpaint = app_.paint_panel_->in_weight_paint_();
        const std::string mode = terrain ? "Terrain Mode" : tiles ? "Tile Mode" : painting_instances ? "Instance Paint"
                               : sculpting ? "Sculpt Mode" : vpaint ? "Vertex Paint" : wpaint ? "Weight Paint"
                               : mesh_edit ? "Edit Mode"
                               : app_.curve_pen_panel_->in_pen_mode_() ? "Pen Mode" : "Object Mode";
        const float w = ctx.text_width(mode) + bh + 26;
        const imm::Box mb{x, hb.y + 3, w, bh};
        bool hov = false, held = false;
        if (ctx.invisible_button("mode_dd", mb, &hov, &held)) ctx.open_popup("mode_menu", glm::vec2(mb.x, mb.bottom() + 2));
        ctx.fill_rounded(mb, hov ? ctx.style.button_hover : ctx.style.button);
        ctx.icon(terrain || tiles ? I::Terrain : painting_instances ? I::BrushDraw
                 : sculpting ? I::SculptMode : vpaint ? I::VertexPaint : wpaint ? I::WeightPaint
                 : mesh_edit ? I::EditMode : I::ObjectMode,
                 {mb.x + 4, mb.y + 2, bh - 4, bh - 4}, ctx.style.text);
        ctx.text_in({mb.x + bh + 2, mb.y, w - bh - 16, bh}, mode, ctx.style.text, 0.0f);
        ctx.arrow({mb.right() - 14, mb.y + 4, 10, bh - 8}, true, ctx.style.text_dim);
        ctx.tooltip("Mode\nObject Mode / Edit Mode (Tab) / Sculpt / Vertex Paint / Weight Paint (Ctrl Tab for the menu)");
        x += w + 8;
        if (ctx.begin_popup("mode_menu", 170)) {
            draw_mode_menu_items_(ctx);
            ctx.end_popup();
        }
        // Edit / Sculpt Mode isolation (Blender's Local View, entered automatically).
        if (app_.edit_object_ && !app_.asset_view_()) {
            if (ctx.icon_button("isolate", I::LocalView, "Isolate in Edit Mode\nEdit and Sculpt Mode hide the other objects and frame "
                                "this one (lights stay). Saved as a preference.", app_.isolate_in_edit_, bh, imm::Context::kAll,
                                imm::Box{x, hb.y + 3, bh, bh}, true)) {
                app_.set_isolate_in_edit(!app_.isolate_in_edit_);
            }
            x += bh + 8;
        }
        if (terrain) x = app_.terrain_panel_->draw_terrain_header_(ctx, x, hb);
        else if (painting_instances) x = app_.instance_paint_panel_->draw_instance_header_(ctx, x, hb);
        else if (tiles) x = app_.tile_mode_panel_->draw_tile_header_(ctx, x, hb);
        else if (sculpting) x = app_.sculpt_panel_->draw_sculpt_header_(ctx, x, hb);
        else if (vpaint || wpaint) x = app_.paint_panel_->draw_paint_header_(ctx, x, hb);
    }
    if (mesh_edit) {
        int sm = static_cast<int>(app_.mesh_.selection.mode);
        if (ctx.icon_group("selmode", {{I::Vertex, "Vertex Select\n1"}, {I::Edge, "Edge Select\n2"}, {I::Face, "Face Select\n3"}}, &sm,
                           imm::Box{x, hb.y + 3, bh * 3, bh}, bh)) {
            if (static_cast<SelectMode>(sm) != app_.mesh_.selection.mode) convert_selection(app_.mesh_.mesh, app_.mesh_.selection, static_cast<SelectMode>(sm));
        }
        x += bh * 3 + 8;
    }
    // Menus.
    const float menus_w = 220;
    ctx.begin_menubar({x, hb.y, menus_w, hb.h}, false);
    draw_view_menu_(ctx);
    if (app_.paint_panel_->in_paint_mode_()) app_.paint_panel_->draw_paint_menu_(ctx);
    if (!app_.paint_panel_->in_brush_mode_() && !preview_only_view_()) {
        draw_select_menu_(ctx, mesh_edit);
        if (!mesh_edit && ctx.begin_menu("Add")) { draw_add_menu_items_(ctx); ctx.end_menu(); }
        if (mesh_edit) draw_mesh_menu_(ctx);
        else draw_object_menu_(ctx);
    }
    ctx.end_menubar();
    const float after_menus = ctx.menubar_end() + 10;

    // Right side: orientation, snap, overlays, x-ray, shading.
    float rx = hb.right() - 6;
    const float s = bh;
    // Blender's Viewport Shading popover, right of the four buttons.
    const imm::Box sdd{rx - s * 0.7f, hb.y + 3, s * 0.7f, s};
    if (ctx.icon_button("shading_opts_btn", I::ArrowDown, "Viewport Shading\nAmbient occlusion and other display options", false,
                        s * 0.7f, imm::Context::kAll, sdd)) {
        ctx.open_popup("shading_opts", glm::vec2(sdd.right() - 220, sdd.bottom() + 2));
    }
    if (ctx.begin_popup("shading_opts", 250)) draw_shading_popover_(ctx);
    rx -= s * 0.7f + 2;
    rx -= s * 4;
    int sh = static_cast<int>(app_.shading_);
    if (ctx.icon_group("shading", {{I::ShadeWire, "Wireframe\nShift Z toggles, Z for the menu"},
                                   {I::ShadeSolid, "Solid\nLighting-independent shading for modelling"},
                                   {I::ShadeMaterial, "Material Preview\nUnlit material colours and textures"},
                                   {I::ShadeRendered, "Rendered\nThe game's full renderer"}},
                       &sh, imm::Box{rx, hb.y + 3, s * 4, s}, s)) {
        app_.set_shading(static_cast<Shading>(sh));
    }
    rx -= s + 10;
    if (ctx.icon_button("xray", I::XRay, "Toggle X-Ray (Alt Z)\nMake surfaces translucent to see -- and select -- what is behind them", app_.xray_, s, imm::Context::kAll,
                        imm::Box{rx, hb.y + 3, s, s}, true)) {
        app_.xray_ = !app_.xray_;
    }
    // Overlays: the toggle plus its popover (caret).
    rx -= s * 0.7f + 4;
    const imm::Box odd{rx, hb.y + 3, s * 0.7f, s};
    if (ctx.icon_button("overlay_opts_btn", I::ArrowDown, "Viewport Overlays\nWhich overlays draw", false, s * 0.7f, imm::Context::kRight, odd)) {
        ctx.open_popup("overlay_opts", glm::vec2(odd.right() - 220, odd.bottom() + 2));
    }
    if (ctx.begin_popup("overlay_opts", 220)) draw_overlays_popover_(ctx);
    rx -= s;
    if (ctx.icon_button("overlays", I::Overlays, "Overlays\nGrid, origins, light and camera glyphs (caret: choose which)", app_.show_overlays_, s,
                        imm::Context::kLeft, imm::Box{rx, hb.y + 3, s, s}, true)) {
        app_.show_overlays_ = !app_.show_overlays_;
    }
    // Physics colliders: the toggle plus its popover (caret) -- all of them, or the selection's.
    rx -= s * 0.7f + 10;
    const imm::Box cdd{rx, hb.y + 3, s * 0.7f, s};
    if (ctx.icon_button("collider_opts_btn", I::ArrowDown, "Collider Display\nAll colliders, or only the selection's", false,
                        s * 0.7f, imm::Context::kRight, cdd)) {
        ctx.open_popup("collider_opts", glm::vec2(cdd.right() - 220, cdd.bottom() + 2));
    }
    if (ctx.begin_popup("collider_opts", 220)) draw_colliders_popover_(ctx);
    app_.test_rects_["collider_opts_btn"] = cdd;
    rx -= s;
    app_.test_rects_["colliders"] = imm::Box{rx, hb.y + 3, s, s};
    if (ctx.icon_button("colliders", I::Collider,
                        app_.collider_view_ == EditorApp::ColliderView::All
                            ? "Physics Colliders\nDraw every collider in the scene (caret: only the selection's)"
                            : "Physics Colliders\nDraw the selected objects' colliders and their children's (caret: all)",
                        app_.show_colliders_, s, imm::Context::kLeft, imm::Box{rx, hb.y + 3, s, s}, true)) {
        app_.show_colliders_ = !app_.show_colliders_;
    }
    // Snap and orientation only steer transforms: the brush modes have none, so they make room there.
    if (!app_.paint_panel_->in_brush_mode_()) {
        rx -= s + 10;
        if (ctx.icon_button("snap", I::Snap, "Snap\nSnap transforms to increments (Ctrl inverts while dragging)", app_.snap_on_, s, imm::Context::kAll,
                            imm::Box{rx, hb.y + 3, s, s}, true)) {
            app_.snap_on_ = !app_.snap_on_;
        }
        // Proportional editing (Edit Mode): the toggle, and its options under the caret.
        if (mesh_edit) {
            rx -= s * 0.7f + 4;
            const imm::Box pdd{rx, hb.y + 3, s * 0.7f, s};
            if (ctx.icon_button("prop_opts_btn", I::ArrowDown, "Proportional Editing Options\nFalloff, connected only, projected, radius",
                                false, s * 0.7f, imm::Context::kRight, pdd)) {
                ctx.open_popup("prop_opts", glm::vec2(pdd.right() - 230, pdd.bottom() + 2));
            }
            app_.test_rects_["prop_opts_btn"] = pdd;
            if (ctx.begin_popup("prop_opts", 230)) draw_proportional_popover_(ctx);
            rx -= s;
            const imm::Box pb{rx, hb.y + 3, s, s};
            if (ctx.icon_button("proportional", I::Proportional,
                                "Proportional Editing (O)\nMoves, rotations and scales also drag the unselected vertices "
                                "inside a radius, fading out toward its edge. While transforming, the mouse wheel "
                                "(or Page Up / Page Down) resizes the circle. Shift O cycles the falloff.",
                                app_.proportional_.enabled, s, imm::Context::kLeft, pb, true)) {
                app_.toggle_proportional();
            }
            app_.test_rects_["proportional"] = pb;
        }
        rx -= 92;
        const imm::Box ob{rx, hb.y + 3, 88, s};
        bool oh = false, oheld = false;
        if (ctx.invisible_button("orient_dd", ob, &oh, &oheld)) ctx.open_popup("orient_menu", glm::vec2(ob.x, ob.bottom() + 2));
        ctx.fill_rounded(ob, oh ? ctx.style.button_hover : ctx.style.button);
        ctx.icon(I::Orientation, {ob.x + 3, ob.y + 2, s - 4, s - 4}, ctx.style.text);
        static const char* kOrientNames[] = {"Global", "Local", "Normal"};
        ctx.text_in({ob.x + s, ob.y, ob.w - s - 12, s}, kOrientNames[static_cast<int>(app_.orient_)], ctx.style.text, 0.0f);
        ctx.icon(I::ArrowDown, {ob.right() - 13, ob.y + 5, 9, s - 10}, ctx.style.text_dim);
        ctx.tooltip("Transform Orientation\nAxes the gizmo and a second X / Y / Z press use: Global, the object's Local axes, "
                    "or the selection's Normal (edit mode)");
        if (ctx.begin_popup("orient_menu", 150)) {
            for (int i = 0; i < 3; ++i) {
                bool on = static_cast<int>(app_.orient_) == i;
                if (ctx.menu_item(kOrientNames[i], "", &on, i != 2 || mesh_edit)) app_.orient_ = static_cast<EditorApp::Orientation>(i);
            }
            ctx.end_popup();
        }
    }
    // Edit Mode symmetry (Blender's X / Y / Z mirror toggles + Local / Global), between the
    // menus and the right-hand cluster when it fits; Properties > Tool > Symmetry always has it.
    // Sculpt Mode's stroke symmetry goes in the same place (Properties > Tool > Brush has it too).
    const bool sculpting = app_.in_sculpt_mode_(), painting = app_.paint_panel_->in_paint_mode_();
    MirrorSettings& header_sym = sculpting ? app_.sculpt_.symmetry : painting ? app_.paint_panel_->paint_settings_().symmetry : app_.edit_symmetry_;
    if ((mesh_edit || sculpting || painting) && after_menus + app_.sculpt_panel_->symmetry_buttons_width_(ctx, hb, header_sym) <= rx - 8) {
        if (sculpting || painting) app_.sculpt_panel_->draw_symmetry_buttons_(ctx, after_menus, hb, header_sym, sculpting ? "sc_sym" : "pt_sym", "strokes");
        else app_.sculpt_panel_->draw_symmetry_buttons_(ctx, after_menus, hb, app_.edit_symmetry_, "ed_sym", "moves, rotations and scales");
    }
}

void ViewportChromePanel::draw_proportional_popover_(imm::Context& ctx) {
    ctx.label_dim("Proportional Editing");
    bool on = app_.proportional_.enabled;
    if (ctx.checkbox("Enabled  (O)", &on)) app_.proportional_.enabled = on;
    ctx.spacing(2);
    ctx.label_dim("Falloff  (Shift O cycles)");
    for (int i = 0; i < kFalloffCount; ++i) {
        const Falloff f = static_cast<Falloff>(i);
        ctx.push_id(i);
        if (ctx.selectable(falloff_name(f), app_.proportional_.falloff == f)) app_.proportional_.falloff = f;
        app_.test_rects_[std::string("prop_falloff:") + falloff_name(f)] = ctx.last_rect();
        // A small plot of the curve at the row's right.
        const imm::Box r = ctx.last_rect();
        const imm::Box plot{r.right() - 46, r.y + 4, 40, r.h - 8};
        glm::vec2 prev{plot.x, plot.bottom() - falloff_weight(f, 0.0f) * plot.h};
        for (int k = 1; k <= 16; ++k) {
            const float t = k / 16.0f;
            const glm::vec2 p{plot.x + t * plot.w, plot.bottom() - falloff_weight(f, std::min(t, 0.999f)) * plot.h};
            ctx.line(prev, p, ctx.style.text_dim, 1.2f);
            prev = p;
        }
        ctx.pop_id();
    }
    ctx.spacing(2);
    bool connected = app_.proportional_.connected, projected = app_.proportional_.projected;
    if (ctx.checkbox("Connected Only", &connected)) app_.proportional_.connected = connected;
    ctx.tooltip("Connected Only\nMeasure along the mesh's edges: vertices on separate pieces never move together");
    if (ctx.checkbox("Projected from View", &projected)) app_.proportional_.projected = projected;
    ctx.tooltip("Projected from View\nMeasure on screen: what sits inside the circle moves, however far behind. "
                "Off: straight-line distance in 3D. (Connected Only overrides it.)");
    float r = app_.proportional_.radius;
    if (ctx.drag_float("Radius", &r, 0.01f, ProportionalSettings::kMinRadius, ProportionalSettings::kMaxRadius)) {
        app_.proportional_.set_radius(r);
    }
    ctx.tooltip("Radius\nIn world units; the mouse wheel resizes it while transforming");
    ctx.end_popup();
}

void ViewportChromePanel::draw_view_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.begin_menu("View")) return;
    bool t = app_.show_toolbar_, n = app_.show_sidebar_;
    if (ctx.menu_item("Toolbar", "T", &t)) app_.show_toolbar_ = !app_.show_toolbar_;
    if (ctx.menu_item("Sidebar", "N", &n)) app_.show_sidebar_ = !app_.show_sidebar_;
    ctx.menu_separator();
    if (ctx.menu_item("Frame Selected", "F / .", nullptr, true, I::Zoom)) app_.frame_selected();
    if (ctx.menu_item("Frame All", "Home", nullptr, true, I::Zoom)) app_.frame_all();
    if (ctx.menu_item(app_.camera_.ortho ? "Perspective" : "Orthographic", "Num 5", nullptr, true, app_.camera_.ortho ? I::Persp : I::Ortho)) {
        app_.camera_.set_ortho(!app_.camera_.ortho);
    }
    if (ctx.begin_menu("Viewpoint", true, I::ViewCamera)) {
        if (ctx.menu_item("Camera", "Num 0", nullptr, true, I::Camera)) app_.view_through_scene_camera_();
        if (ctx.menu_item("Top", "Num 7")) app_.camera_.axis_view('t', false);
        if (ctx.menu_item("Bottom", "Ctrl Num 7")) app_.camera_.axis_view('t', true);
        if (ctx.menu_item("Front", "Num 1")) app_.camera_.axis_view('f', false);
        if (ctx.menu_item("Back", "Ctrl Num 1")) app_.camera_.axis_view('f', true);
        if (ctx.menu_item("Right", "Num 3")) app_.camera_.axis_view('r', false);
        if (ctx.menu_item("Left", "Ctrl Num 3")) app_.camera_.axis_view('r', true);
        ctx.end_menu();
    }
    if (ctx.menu_item("Align Active Camera to View", "Ctrl Alt Num 0", nullptr, !app_.asset_view_(), I::Camera)) app_.align_scene_camera_to_view_();
    ctx.menu_separator();
    if (ctx.menu_item("Toggle Maximize Area", "Ctrl Space")) app_.maximized_ = !app_.maximized_;
    ctx.menu_separator();
    // The engine's debug HUD, drawn inside the viewport (F3 there cycles off / fps / full).
    bool stats = app_.engine_.debug_overlay().visible();
    if (ctx.menu_item("Stats Overlay", "F3", &stats)) app_.toggle_stats_overlay();
    ctx.end_menu();
}

void ViewportChromePanel::draw_select_menu_(imm::Context& ctx, bool mesh_edit) {
    using I = imm::Icon;
    if (!ctx.begin_menu("Select")) return;
    if (ctx.menu_item("All", "A")) {
        if (mesh_edit) app_.mesh_.selection.select_all(app_.mesh_.mesh);
        else { app_.doc_.clear_selection(); for (ObjectId i : app_.visible_ids_()) app_.doc_.select(i, true); }
    }
    if (ctx.menu_item("None", "Alt A")) { if (mesh_edit) app_.mesh_.selection.clear(); else app_.doc_.clear_selection(); }
    if (ctx.menu_item("Invert", "Ctrl I")) {
        if (mesh_edit) app_.invert_mesh_selection_();
        else {
            std::vector<ObjectId> inv;
            for (ObjectId i : app_.visible_ids_()) if (!app_.doc_.is_selected(i)) inv.push_back(i);
            app_.doc_.clear_selection();
            for (ObjectId i : inv) app_.doc_.select(i, true);
        }
    }
    ctx.menu_separator();
    if (ctx.menu_item("Box Select", "B", nullptr, true, I::SelectBox)) app_.tool_ = EditorApp::Tool::Select;
    if (mesh_edit && ctx.menu_item("Select Linked", "Ctrl L", nullptr, true, I::Link)) select_linked(app_.mesh_.mesh, app_.mesh_.selection);
    ctx.end_menu();
}

void ViewportChromePanel::draw_add_menu_items_(imm::Context& ctx) {
    using I = imm::Icon;
    static const I prim_icons[] = {I::Cube, I::Plane, I::Cylinder, I::Sphere, I::Plane};
    if (ctx.begin_menu("Mesh", true, I::Mesh)) {
        const auto& names = primitive_names();
        for (size_t i = 0; i < names.size(); ++i) if (ctx.menu_item(names[i], "", nullptr, true, prim_icons[i % 5])) app_.create_primitive(names[i]);
        ctx.end_menu();
    }
    if (ctx.begin_menu("Light", true, I::PointLight)) {
        if (ctx.menu_item("Sun", "", nullptr, true, I::Sun)) app_.create_with_component("DirectionalLight", "Sun");
        if (ctx.menu_item("Point", "", nullptr, true, I::PointLight)) app_.create_with_component("PointLight", "Point Light");
        if (ctx.menu_item("Spot", "", nullptr, true, I::SpotLight)) app_.create_with_component("SpotLight", "Spot Light");
        if (ctx.menu_item("Environment", "", nullptr, true, I::EnvLight)) app_.create_with_component("EnvironmentLight", "Environment");
        ctx.end_menu();
    }
    if (ctx.menu_item("Camera", "", nullptr, true, I::Camera)) app_.create_with_component("Camera", "Camera");
    if (ctx.menu_item("Empty", "", nullptr, true, I::Empty)) app_.create_empty();
    ctx.menu_separator();
    if (ctx.menu_item("Reflection Probe", "", nullptr, true, I::EnvLight)) app_.create_with_component("ReflectionProbe", "Reflection Probe");
    if (ctx.menu_item("Heightmap Terrain", "", nullptr, true, I::Terrain)) app_.create_with_component("HeightmapTerrain", "Terrain");
    if (ctx.menu_item("Tile Terrain", "", nullptr, true, I::Terrain)) app_.create_with_component("TileTerrain", "Tile Terrain");
    if (ctx.menu_item("Planet", "", nullptr, true, I::World)) app_.create_planet();
    if (ctx.menu_item("Instance Painter", "", nullptr, true, I::BrushDraw)) {
        app_.create_with_component("InstancePainter", "Painted Instances");
    }
    ctx.tooltip("Instance Painter\nA bag of brush-placed copies of your object assets -- trees, "
                "rocks, clutter. Paint them onto any surface in the scene in Instance Paint Mode.");
}

void ViewportChromePanel::draw_object_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.begin_menu("Object")) return;
    const bool any = !app_.doc_.selection().empty() && !app_.playing() && !app_.asset_view_();
    if (ctx.begin_menu("Transform", any, I::Move)) {
        if (ctx.menu_item("Move", "G", nullptr, true, I::Move)) app_.pending_modal_kind_ = ModalKind::Grab;
        if (ctx.menu_item("Rotate", "R", nullptr, true, I::Rotate)) app_.pending_modal_kind_ = ModalKind::Rotate;
        if (ctx.menu_item("Scale", "S", nullptr, true, I::Scale)) app_.pending_modal_kind_ = ModalKind::Scale;
        ctx.end_menu();
    }
    if (ctx.begin_menu("Clear", any)) {
        if (ctx.menu_item("Location", "Alt G")) app_.clear_transform_(0);
        if (ctx.menu_item("Rotation", "Alt R")) app_.clear_transform_(1);
        if (ctx.menu_item("Scale", "Alt S")) app_.clear_transform_(2);
        ctx.end_menu();
    }
    if (ctx.begin_menu("Snap", true, I::Snap)) {
        if (ctx.menu_item("Cursor to Selected")) { glm::vec3 p; glm::mat3 b; if (app_.gizmo_target_(false, p, b)) app_.cursor3d_ = p; }
        if (ctx.menu_item("Cursor to World Origin")) app_.cursor3d_ = glm::vec3(0.0f);
        if (ctx.menu_item("Selection to Cursor", "", nullptr, any)) app_.selection_to_cursor_();
        ctx.end_menu();
    }
    if (ctx.begin_menu("Instance", any && app_.instances_panel_->in_instance_(app_.doc_.primary()), I::Link)) {
        app_.instances_panel_->draw_instance_menu_items_(ctx, app_.doc_.primary());
        ctx.end_menu();
    }
    if (ctx.begin_menu("Parent", any, I::Link)) {
        if (ctx.menu_item("Object (keep transform)", "Ctrl P", nullptr, app_.doc_.selection().size() > 1)) app_.parent_selection_to_active_();
        ctx.menu_separator();
        ObjectId bind_skin = 0, bind_rig = 0;
        const bool bindable = app_.weights_panel_->bind_targets_(bind_skin, bind_rig);
        if (ctx.menu_item("Bind to Rig with Automatic Weights", "", nullptr, bindable, I::WeightPaint)) {
            app_.weights_panel_->bind_to_rig_(bind_skin, bind_rig, BindWeights::Automatic);
        }
        ctx.tooltip("Bind to Rig with Automatic Weights\nMake the mesh follow the rig under it: a vertex group per bone,\n"
                    "weighted by how the bone's heat spreads over the surface (Blender's automatic weights)");
        if (ctx.menu_item("Bind to Rig with Envelope Weights", "", nullptr, bindable, I::WeightPaint)) {
            app_.weights_panel_->bind_to_rig_(bind_skin, bind_rig, BindWeights::Envelope);
        }
        ctx.tooltip("Bind to Rig with Envelope Weights\nWeights from plain distance to each bone -- rougher than automatic\n"
                    "weights, but it needs no closed surface");
        if (ctx.menu_item("Bind to Rig with Empty Groups", "", nullptr, bindable)) {
            app_.weights_panel_->bind_to_rig_(bind_skin, bind_rig, BindWeights::EmptyGroups);
        }
        ctx.tooltip("Bind to Rig with Empty Groups\nOne vertex group per bone and no weights at all, to paint by hand");
        ctx.menu_separator();
        if (ctx.menu_item("Clear Parent (keep transform)", "Alt P")) app_.clear_parent_keep_transform_();
        ctx.end_menu();
    }
    ctx.menu_separator();
    if (ctx.menu_item("Duplicate Objects", "Shift D", nullptr, any, I::Duplicate)) { app_.duplicate_selected(); app_.pending_modal_kind_ = ModalKind::Grab; }
    if (ctx.menu_item("Delete", "X", nullptr, any, I::Trash)) app_.delete_selected();
    ctx.menu_separator();
    if (ctx.menu_item("Shade Smooth", "", nullptr, any)) app_.shade_selected_(true);
    ctx.tooltip("Shade Smooth\nInterpolate normals across every face of the selected objects' meshes");
    if (ctx.menu_item("Shade Flat", "", nullptr, any)) app_.shade_selected_(false);
    ctx.tooltip("Shade Flat\nOne normal per face: the faces' edges show");
    ctx.menu_separator();
    if (ctx.menu_item("Hide Selected", "H", nullptr, any, I::EyeClosed)) app_.hide_(app_.doc_.selection());
    if (ctx.menu_item("Show Hidden", "Alt H", nullptr, true, I::Eye)) app_.unhide_all_();
    ctx.end_menu();
}

void ViewportChromePanel::draw_mirror_menu_items_(imm::Context& ctx) {
    static const char* kAxes[] = {"X", "Y", "Z"};
    for (int global = 1; global >= 0; --global) {
        for (int a = 0; a < 3; ++a) {
            const std::string label = std::string(kAxes[a]) + (global ? " Global" : " Local");
            if (ctx.menu_item(label)) app_.mesh_tools_panel_->mirror_mesh_selection_(a, global != 0);
            ctx.tooltip(std::string("Mirror ") + label + "\nFlip the selection along the " + (global ? "world's " : "mesh's ") +
                        kAxes[a] + " axis, through the selection's centre");
        }
        if (global) ctx.menu_separator();
    }
}

void ViewportChromePanel::draw_mesh_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.begin_menu("Mesh")) return;
    auto op = [&](const char* label, auto&& fn) { app_.mesh_.edit(label, fn); };
    if (ctx.begin_menu("Transform", true, I::Move)) {
        if (ctx.menu_item("Move", "G", nullptr, true, I::Move)) app_.pending_modal_kind_ = ModalKind::Grab;
        if (ctx.menu_item("Rotate", "R", nullptr, true, I::Rotate)) app_.pending_modal_kind_ = ModalKind::Rotate;
        if (ctx.menu_item("Scale", "S", nullptr, true, I::Scale)) app_.pending_modal_kind_ = ModalKind::Scale;
        ctx.end_menu();
    }
    if (ctx.begin_menu("Mirror", true)) {
        draw_mirror_menu_items_(ctx);
        ctx.end_menu();
    }
    ctx.menu_separator();
    if (ctx.menu_item("Extrude", "E", nullptr, true, I::Extrude)) app_.pending_extrude_ = true;
    if (ctx.menu_item("Inset Faces", "I", nullptr, true, I::Inset)) app_.pending_modal_kind_ = ModalKind::Inset;
    if (ctx.menu_item("Bevel Edges", "Ctrl B", nullptr, true, I::Bevel)) app_.pending_modal_kind_ = ModalKind::Bevel;
    if (ctx.menu_item("Make Face", "F", nullptr, true, I::Face)) op("Make Face", [](EditMesh& m, MeshSelection& s) { fill_face(m, s); });
    if (ctx.menu_item("Duplicate", "Shift D", nullptr, true, I::Duplicate)) {
        op("Duplicate", [](EditMesh& m, MeshSelection& s) { duplicate_faces(m, s); });
        app_.pending_modal_kind_ = ModalKind::Grab;
    }
    ctx.menu_separator();
    if (ctx.begin_menu("Merge", true)) {
        if (ctx.menu_item("At Center", "M")) op("Merge", [](EditMesh& m, MeshSelection& s) { merge_at_center(m, s); });
        if (ctx.menu_item("By Distance")) op("Merge by Distance", [this](EditMesh& m, MeshSelection& s) { merge_by_distance(m, s, app_.merge_dist_); });
        ctx.end_menu();
    }
    if (ctx.begin_menu("Delete", true, I::Trash)) {
        auto del = [&](SelectMode as) { op("Delete", [as](EditMesh& m, MeshSelection& s) { convert_selection(m, s, as); delete_selection(m, s); }); };
        if (ctx.menu_item("Vertices", "", nullptr, true, I::Vertex)) del(SelectMode::Vertex);
        if (ctx.menu_item("Edges", "", nullptr, true, I::Edge)) del(SelectMode::Edge);
        if (ctx.menu_item("Faces", "", nullptr, true, I::Face)) del(SelectMode::Face);
        ctx.end_menu();
    }
    if (ctx.begin_menu("Normals", true)) {
        if (ctx.menu_item("Flip", "Alt N")) op("Flip Normals", [](EditMesh& m, MeshSelection& s) { flip_normals(m, s); });
        if (ctx.menu_item("Shade Smooth")) op("Shade Smooth", [](EditMesh& m, MeshSelection& s) { set_smooth(m, s, true); });
        if (ctx.menu_item("Shade Flat")) op("Shade Flat", [](EditMesh& m, MeshSelection& s) { set_smooth(m, s, false); });
        ctx.end_menu();
    }
    if (ctx.begin_menu("UV Unwrap", true, I::Texture)) {
        if (ctx.menu_item("Cube Projection", "U")) op("Box UV", [this](EditMesh& m, MeshSelection& s) { uv_box_project(m, s, app_.uv_scale_); });
        if (ctx.menu_item("Project X")) op("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 0); });
        if (ctx.menu_item("Project Y")) op("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 1); });
        if (ctx.menu_item("Project Z")) op("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 2); });
        ctx.end_menu();
    }
    ctx.end_menu();
}

void ViewportChromePanel::draw_toolbar_(imm::Context& ctx, bool mesh_edit) {
    using I = imm::Icon;
    const imm::Box r = toolbar_rect_(mesh_edit);
    ctx.fill_rounded(r, app_.et_.viewport.toolbar_bg, 6);
    float y = r.y + 4;
    auto tool_btn = [&](const char* id, I ic, const char* tip, bool on) {
        const imm::Box b{r.x + 3, y, kToolSize, kToolSize};
        y += kToolSize + 2;
        return ctx.icon_button(id, ic, tip, on, kToolSize, imm::Context::kAll, b);
    };
    if (app_.terrain_panel_->in_terrain_mode_()) { app_.terrain_panel_->draw_terrain_toolbar_(ctx, r); return; }
    if (app_.instance_paint_panel_->in_instance_mode_()) { app_.instance_paint_panel_->draw_instance_toolbar_(ctx, r); return; }
    if (app_.tile_mode_panel_->in_tile_mode_()) { app_.tile_mode_panel_->draw_tile_toolbar_(ctx, r); return; }
    if (app_.in_sculpt_mode_()) { app_.sculpt_panel_->draw_sculpt_toolbar_(ctx, r); return; }
    if (app_.paint_panel_->in_paint_mode_()) { app_.paint_panel_->draw_paint_toolbar_(ctx, r); return; }
    if (app_.curve_pen_panel_->in_pen_mode_()) { app_.curve_pen_panel_->draw_pen_toolbar_(ctx, r); return; }
    if (tool_btn("t_select", I::SelectBox, "Select Box\nClick or drag to select (Shift adds, Ctrl removes)", app_.tool_ == EditorApp::Tool::Select)) app_.tool_ = EditorApp::Tool::Select;
    if (tool_btn("t_cursor", I::Cursor, "Cursor\nClick to place the 3D cursor (Shift RMB anywhere)", app_.tool_ == EditorApp::Tool::Cursor)) app_.tool_ = EditorApp::Tool::Cursor;
    // The Pen lives here rather than only in the mode menu: it is a TOOL, and this is where a
    // tool is looked for. It needs nothing of the object -- entering adds a Curve when there is
    // none, which is what "draw a path on this" means.
    if (!mesh_edit) {
        const bool pen_on = app_.curve_pen_panel_->in_pen_mode_();
        if (tool_btn("t_pen", I::Pen,
                     "Pen\nDraw a curve on this object. Click to add a point, click the line to "
                     "insert one, drag a point or its handles. X deletes, C closes the loop.",
                     pen_on)) {
            if (pen_on) app_.curve_pen_panel_->end_pen_();
            else if (app_.doc_.primary()) {
                app_.curve_pen_panel_->begin_pen_(app_.doc_.primary());
                app_.curve_pen_panel_->set_tool(CurvePenPanel::PenTool::Draw);
            }
        }
    }
    y += 6;
    if (tool_btn("t_move", I::Move, "Move\nGizmo for moving (G for the modal move)", app_.tool_ == EditorApp::Tool::Move)) app_.tool_ = EditorApp::Tool::Move;
    if (tool_btn("t_rotate", I::Rotate, "Rotate\nGizmo for rotating (R for the modal rotate)", app_.tool_ == EditorApp::Tool::Rotate)) app_.tool_ = EditorApp::Tool::Rotate;
    if (tool_btn("t_scale", I::Scale, "Scale\nGizmo for scaling (S for the modal scale)", app_.tool_ == EditorApp::Tool::Scale)) app_.tool_ = EditorApp::Tool::Scale;
    if (mesh_edit) {
        y += 6;
        if (tool_btn("t_extrude", I::Extrude, "Extrude Region\nExtrude the selection, then move it (E)", false)) app_.pending_extrude_ = true;
        if (tool_btn("t_inset", I::Inset, "Inset Faces\nInset the selected faces; move the mouse to size (I)", false)) app_.pending_modal_kind_ = ModalKind::Inset;
        if (tool_btn("t_bevel", I::Bevel, "Bevel\nBevel the selected edges; move the mouse to size (Ctrl B)", false)) app_.pending_modal_kind_ = ModalKind::Bevel;
        if (tool_btn("t_loopcut", I::LoopCut, "Loop Cut and Slide\nHover an edge ring, wheel for more cuts, click to cut and slide (Ctrl R)",
                     app_.loopcut_.active)) {
            app_.mesh_tools_panel_->begin_loop_cut();
        }
    }
}

void ViewportChromePanel::draw_nav_gizmo_(imm::Context& ctx) {
    using I = imm::Icon;
    const imm::Box g = nav_gizmo_rect_();
    const glm::vec2 c{g.x + 55, g.y + 55};
    const float R = 40.0f;
    const glm::mat3 vr = glm::mat3(app_.view_proj_() ? app_.view_proj_()->view : glm::mat4(1.0f));
    // Orbit by dragging the ball's background.
    bool hov = false, held = false;
    ctx.invisible_button("navball", {c.x - 50, c.y - 50, 100, 100}, &hov, &held);
    if (held && ctx.input().mouse_delta != glm::vec2(0.0f)) app_.camera_.orbit(ctx.input().mouse_delta);
    if (hov || held) ctx.circle(c, 50, app_.et_.viewport.nav_hover);
    struct Ball { glm::vec3 axis; int idx; bool pos; float depth; glm::vec2 p; };
    std::vector<Ball> balls;
    for (int i = 0; i < 3; ++i) {
        for (int sgn = 0; sgn < 2; ++sgn) {
            glm::vec3 a(0.0f);
            a[i] = sgn == 0 ? 1.0f : -1.0f;
            const glm::vec3 v = vr * a;
            balls.push_back({a, i, sgn == 0, v.z, c + glm::vec2(v.x, -v.y) * R});
        }
    }
    std::sort(balls.begin(), balls.end(), [](const Ball& a, const Ball& b) { return a.depth < b.depth; });
    const glm::vec4 cols[3] = {ctx.style.axis_x, ctx.style.axis_y, ctx.style.axis_z};
    const char* names[3] = {"X", "Y", "Z"};
    int clicked = -1;
    bool clicked_pos = true;
    for (const auto& b : balls) {
        const glm::vec4 col = cols[b.idx];
        const bool over = glm::distance(ctx.mouse(), b.p) < 10.0f && hov;
        if (b.pos) {
            ctx.line(c, b.p, imm::with_alpha(col, 0.9f), 2.0f);
            ctx.circle(b.p, over ? 10.5f : 9.0f, col);
            ctx.text_in({b.p.x - 8, b.p.y - 8, 16, 16}, names[b.idx], app_.et_.viewport.nav_label, 0, true);
        } else {
            ctx.circle(b.p, over ? 9.0f : 7.5f, imm::with_alpha(col, over ? 0.7f : 0.35f));
            ctx.ring(b.p, 7.5f, 1.2f, imm::with_alpha(col, 0.8f));
        }
        if (over && ctx.input().released[0] && !app_.nav_ball_dragged_) { clicked = b.idx; clicked_pos = b.pos; }
    }
    if (held && glm::length(ctx.input().mouse_delta) > 0.0f) app_.nav_ball_dragged_ = true;
    if (!ctx.input().down[0]) app_.nav_ball_dragged_ = false;
    if (clicked >= 0) {
        // A ball looks FROM that axis, as in Blender.
        if (clicked == 0) app_.camera_.axis_view('r', !clicked_pos);
        else if (clicked == 1) app_.camera_.axis_view('f', clicked_pos);
        else app_.camera_.axis_view('t', !clicked_pos);
    }
    // Zoom / pan drag buttons, camera and projection toggles.
    float y = g.y + 114;
    const float s = 26;
    const imm::Box zb{g.x + 42, y, s, s}, pb{g.x + 42, y + 30, s, s}, cb{g.x + 42, y + 60, s, s}, ob{g.x + 42, y + 90, s, s};
    ctx.fill_rounded({zb.x - 2, zb.y - 2, s + 4, s * 4 + 4 * 3 + 4}, app_.et_.viewport.nav_backdrop, (s + 4) * 0.5f);
    bool zh = false, zheld = false, ph = false, pheld = false;
    ctx.invisible_button("nav_zoom", zb, &zh, &zheld);
    if (zh || zheld) ctx.circle(zb.center(), s * 0.5f, app_.et_.viewport.nav_hover);
    ctx.icon(I::Zoom, zb.shrink(5), ctx.style.text);
    ctx.tooltip("Zoom\nDrag up and down to zoom the view");
    if (zheld) app_.camera_.dolly(-ctx.input().mouse_delta.y * 0.04f);
    ctx.invisible_button("nav_pan", pb, &ph, &pheld);
    if (ph || pheld) ctx.circle(pb.center(), s * 0.5f, app_.et_.viewport.nav_hover);
    ctx.icon(I::Hand, pb.shrink(5), ctx.style.text);
    ctx.tooltip("Move\nDrag to pan the view");
    if (pheld) app_.camera_.pan(ctx.input().mouse_delta, app_.viewport_box_.h);
    if (ctx.icon_button("nav_cam", I::Camera, "Toggle Camera View\nLook through the scene camera (Numpad 0)", false, s, imm::Context::kAll, cb)) {
        app_.view_through_scene_camera_();
    }
    if (ctx.icon_button("nav_ortho", app_.camera_.ortho ? I::Ortho : I::Persp, "Switch Projection\nPerspective / orthographic (Numpad 5)", false, s,
                        imm::Context::kAll, ob)) {
        app_.camera_.set_ortho(!app_.camera_.ortho);
    }
}

void ViewportChromePanel::draw_sidebar_(imm::Context& ctx, bool mesh_edit) {
    using I = imm::Icon;
    const imm::Box r = sidebar_rect_();
    ctx.fill_rounded(r, app_.et_.viewport.sidebar_bg, 6, imm::Context::kLeft);
    static const std::vector<std::string> tabs = {"Item", "View"};
    ctx.tab_bar("sidebar_tabs", {r.x, r.y, r.w, 26}, tabs, &app_.sidebar_tab_, nullptr, false);
    ctx.begin_region("sidebar", {r.x, r.y + 26, r.w, r.h - 26}, true);
    const float saved_ratio = ctx.style.label_ratio;
    ctx.style.label_ratio = 0.34f;
    if (app_.sidebar_tab_ == 0) {
        if (mesh_edit) {
            if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
                const auto vs = app_.mesh_.selection.affected_vertices(app_.mesh_.mesh);
                if (vs.empty()) ctx.label_dim("Nothing selected");
                else {
                    glm::vec3 c = selection_center(app_.mesh_.mesh, app_.mesh_.selection);
                    glm::vec3 nc = c;
                    if (ctx.drag_float_stacked("Median", &nc.x, 3, 0.01f, "%.3f m")) {
                        const glm::vec3 d = nc - c;
                        app_.mesh_.edit("Move Vertices", [&](EditMesh& m, MeshSelection& s) { translate_selection(m, s, d); }, "sidebar_median");
                    }
                    if (ctx.last_deactivated()) app_.mesh_.undo.end_merge();
                }
            }
        } else if (const ObjectId id = app_.doc_.primary(); id && app_.doc_.find(id)) {
            if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
                glm::vec3 p, rr, s;
                app_.timeline_panel_->get_object_transform_(id, p, rr, s);
                glm::vec3 np = p, nr = rr, ns = s;
                bool act = false, fin = false;
                const bool a = ctx.drag_float_stacked("Location", &np.x, 3, 0.02f, "%.3f m"); act |= ctx.last_group_active(); fin |= ctx.last_deactivated();
                const bool b = ctx.drag_float_stacked("Rotation", &nr.x, 3, 0.5f, "%.1f"); act |= ctx.last_group_active(); fin |= ctx.last_deactivated();
                const bool cc = ctx.drag_float_stacked("Scale", &ns.x, 3, 0.01f, "%.3f"); act |= ctx.last_group_active(); fin |= ctx.last_deactivated();
                if ((a || b || cc) && !app_.playing()) app_.timeline_panel_->set_object_transform_(id, np, nr, ns, "Transform", act ? "sidebar_transform" : std::string());
                if (fin) app_.doc_.end_merge();
                glm::vec3 lo(1e30f), hi(-1e30f);
                if (app_.object_bounds_(id, lo, hi, true)) {
                    char buf[96];
                    const glm::vec3 d = hi - lo;
                    std::snprintf(buf, sizeof(buf), "%.3f m   %.3f m   %.3f m", d.x, d.y, d.z);
                    imm::Box row = ctx.property_row("Dimensions");
                    ctx.text_in(row, buf, ctx.style.text_dim, 4.0f);
                }
            }
        } else {
            ctx.label_dim("Nothing selected");
        }
    } else {
        if (ctx.collapsing_header("View", true, nullptr, I::ViewCamera)) {
            if (ctx.drag_float("Field of View", &app_.camera_.fov, 0.2f, 5.0f, 150.0f, "%.1f")) app_.camera_.apply();
            if (ctx.drag_float("Distance", &app_.camera_.distance, 0.05f, 0.05f, 5000.0f, "%.2f m")) app_.camera_.apply();
            bool ortho = app_.camera_.ortho;
            if (ctx.property_bool("Orthographic", &ortho)) app_.camera_.set_ortho(ortho);
        }
        if (ctx.collapsing_header("3D Cursor", true, nullptr, I::Cursor)) {
            ctx.drag_float_stacked("Location", &app_.cursor3d_.x, 3, 0.02f, "%.3f m");
        }
    }
    ctx.style.label_ratio = saved_ratio;
    ctx.end_region();
}

std::string ViewportChromePanel::view_name_() const {
    const bool o = app_.camera_.ortho;
    const float p = app_.camera_.pitch_deg;
    const float y = std::fmod(std::fmod(app_.camera_.yaw_deg, 360.0f) + 360.0f, 360.0f);
    auto near = [](float a, float b) { return std::abs(a - b) < 0.6f; };
    std::string name = "User";
    if (near(p, 89.5f)) name = "Top";
    else if (near(p, -89.5f)) name = "Bottom";
    else if (near(p, 0.0f)) {
        if (near(y, 0.0f) || near(y, 360.0f)) name = "Front";
        else if (near(y, 90.0f)) name = "Right";
        else if (near(y, 180.0f)) name = "Back";
        else if (near(y, 270.0f)) name = "Left";
    }
    return name + (o ? " Orthographic" : " Perspective");
}

void ViewportChromePanel::shadow_text_(imm::Context& ctx, glm::vec2 p, const std::string& s, const glm::vec4& c) const {
    ctx.draw_text(p + glm::vec2(1, 1), s, imm::with_alpha(app_.et_.viewport.text_shadow, app_.et_.viewport.text_shadow.a * c.a));
    ctx.draw_text(p, s, c);
}

void ViewportChromePanel::draw_overlay_text_(imm::Context& ctx, const ViewProj& vp) {
    const float x = app_.viewport_box_.x + (app_.show_toolbar_ ? kToolSize + 22 : 10);
    const float y0 = app_.viewport_box_.y + 8;
    shadow_text_(ctx, {x, y0}, view_name_(), app_.et_.viewport.overlay_text);
    (void)vp;
    std::string ctxline = "(1) " + app_.doc_.scene_name();
    if (app_.asset_view_() || app_.active_type_ == AssetType::Object) {
        ctxline = "(" + std::string(app_.assets_panel_->asset_type_info_(app_.active_type_).singular) + ") " + app_.properties_panel_->active_asset_label_();
        if (app_.active_type_ == AssetType::Object && app_.doc_.primary() && app_.doc_.find(app_.doc_.primary())) ctxline += " | " + get_string(*app_.doc_.find(app_.doc_.primary()), "name");
    }
    else if (app_.doc_.primary() && app_.doc_.find(app_.doc_.primary())) ctxline += " | " + get_string(*app_.doc_.find(app_.doc_.primary()), "name");
    shadow_text_(ctx, {x, y0 + 16}, ctxline, imm::with_alpha(app_.et_.viewport.overlay_text, app_.et_.viewport.overlay_text.a * 0.9f));
    if (app_.playing()) shadow_text_(ctx, {x, y0 + 32}, app_.play_scene_ && !app_.play_scene_->is_simulating() ? "PAUSED" : "PLAYING",
                                ctx.style.object_active);
    if (app_.show_overlays_ && app_.show_stats_overlay_) shadow_text_(ctx, {x, y0 + (app_.playing() ? 48.0f : 32.0f)}, app_.statusbar_panel_->scene_stats_(), app_.et_.viewport.overlay_text);
}

namespace {

/**
 * @brief 2D point-in-triangle, for hit-testing a projected glyph face.
 *
 * False for a degenerate (edge-on) triangle: with no area, every point sits on all three
 * sides at once, which would otherwise claim the whole screen for a bone seen end-on.
 */
bool in_triangle(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    const float area2 = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area2) < 1.0f) return false;   // under a square pixel of area
    const float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    const float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    const float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

/**
 * @brief The octahedral bone from `head` to `tail`, projected to the screen.
 *
 * Pose mode's bone display; the origin styles draw axes or an orb, never a bone.
 *
 * `x_hint` rolls the body so it turns with its joint instead of spinning as the direction
 * crosses an axis; `z_hint` is the fallback when the bone runs along `x_hint`.
 *
 * @return False if any corner is behind the camera, or the bone has no length.
 */
bool project_bone(const ViewProj& vp, const glm::vec3& head, const glm::vec3& tail, const glm::vec3& x_hint,
                  const glm::vec3& z_hint, glm::vec2& head_px, glm::vec2& tail_px, glm::vec2 ring_px[4]) {
    const glm::vec3 along = tail - head;
    const float len = glm::length(along);
    if (len < 1e-5f) return false;
    const glm::vec3 dir = along / len;
    glm::vec3 u = x_hint - dir * glm::dot(x_hint, dir);
    if (glm::length(u) < 1e-4f) {
        u = z_hint - dir * glm::dot(z_hint, dir);
        if (glm::length(u) < 1e-4f) u = glm::cross(dir, std::abs(dir.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0));
    }
    u = glm::normalize(u);
    const glm::vec3 v = glm::cross(dir, u);
    const float r = 0.14f * len;
    const glm::vec3 mid = head + dir * r;
    const glm::vec3 ring[4] = {mid + u * r, mid + v * r, mid - u * r, mid - v * r};
    const auto h = vp.project(head), t = vp.project(tail);
    if (!h || !t) return false;
    for (int i = 0; i < 4; ++i) {
        const auto p = vp.project(ring[i]);
        if (!p) return false;
        ring_px[i] = *p;
    }
    head_px = *h;
    tail_px = *t;
    return true;
}

} // namespace

void ViewportChromePanel::draw_object_glyph_(imm::Context& ctx, const ViewProj& vp, ObjectId id, const Node& node, const glm::mat4& w,
                                   const glm::vec4& col) {
    auto P = [&](const glm::vec3& p) { return vp.project(p); };
    auto seg = [&](const glm::vec3& a, const glm::vec3& b) {
        auto pa = P(a), pb = P(b);
        if (pa && pb) ctx.line(*pa, *pb, col, 1.3f);
    };
    const glm::vec3 pos(w[3]);
    const glm::vec3 X = glm::normalize(glm::vec3(w[0])), Y = glm::normalize(glm::vec3(w[1])), Z = glm::normalize(glm::vec3(w[2]));
    std::string kind;
    const Node* comp = nullptr;
    if (!node.contains("components") || !node.at("components").is_sequence()) return;   // a bare prefab instance
    for (const auto& c : node.at("components").as_seq()) {
        const std::string t = component_type(c);
        if (t == "Camera" || t == "DirectionalLight" || t == "PointLight" || t == "SpotLight") { kind = t; comp = &c; break; }
    }
    // Pose mode draws the rig as bones: its own display, not an overlay the user turns off.
    const bool pose_bone = kind.empty() && app_.in_pose_mode_() && app_.timeline_panel_->in_anim_rig_(id);
    // "Cameras / Lights" covers the symbols; "Origins" covers everything else's origin.
    if (!pose_bone && (!kind.empty() ? !app_.show_glyphs_ : !app_.show_origins_)) return;
    const float wpp = vp.world_per_pixel(pos);
    if (kind == "Camera") {
        const float fov = get_float(*comp, "fov", 50.0f);
        const float d = 1.0f, hh = std::tan(glm::radians(fov * 0.5f)) * d, hw = hh * 16.0f / 9.0f;
        const glm::vec3 f = -Z, c0 = pos + f * d;
        const glm::vec3 q[4] = {c0 + X * hw + Y * hh, c0 - X * hw + Y * hh, c0 - X * hw - Y * hh, c0 + X * hw - Y * hh};
        for (int i = 0; i < 4; ++i) { seg(pos, q[i]); seg(q[i], q[(i + 1) % 4]); }
        // The "up" triangle above the frame.
        const glm::vec3 t0 = c0 + Y * hh * 1.1f - X * hw * 0.5f, t1 = c0 + Y * hh * 1.1f + X * hw * 0.5f, t2 = c0 + Y * hh * 1.7f;
        auto a = P(t0), b = P(t1), cpt = P(t2);
        if (a && b && cpt) ctx.triangle(*a, *b, *cpt, imm::with_alpha(col, 0.85f));
        return;
    }
    auto center = P(pos);
    if (!center) return;
    if (kind == "DirectionalLight") {
        ctx.ring(*center, 6, 1.3f, col);
        ctx.circle(*center, 2.5f, col);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 0.7854f;
            ctx.line(*center + glm::vec2(std::cos(a), std::sin(a)) * 9.0f, *center + glm::vec2(std::cos(a), std::sin(a)) * 13.0f, col, 1.2f);
        }
        const glm::vec3 dir = glm::normalize(get_vec3(*comp, "direction", glm::vec3(-0.35f, -0.45f, -0.82f)));
        for (int i = 0; i < 6; ++i) {   // dashed direction line
            if (i % 2) continue;
            seg(pos + dir * (i * 0.4f), pos + dir * ((i + 1) * 0.4f));
        }
        return;
    }
    if (kind == "PointLight") {
        ctx.ring(*center, 8, 1.3f, col);
        ctx.circle(*center, 2.5f, col);
        return;
    }
    if (kind == "SpotLight") {
        const glm::vec3 dir = glm::normalize(get_vec3(*comp, "direction", glm::vec3(0, 0, -1)));
        const float ang = glm::radians(get_float(*comp, "outer_angle", 30.0f));
        const float len = 2.0f, rad = std::tan(ang) * len;
        glm::vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
        glm::vec3 v = glm::cross(dir, u);
        glm::vec3 prev;
        for (int i = 0; i <= 16; ++i) {
            const float a = 6.2831853f * i / 16;
            const glm::vec3 p = pos + dir * len + (u * std::cos(a) + v * std::sin(a)) * rad;
            if (i > 0) seg(prev, p);
            if (i % 4 == 0) seg(pos, p);
            prev = p;
        }
        ctx.circle(*center, 2.5f, col);
        return;
    }
    (void)wpp;
    if (pose_bone) draw_pose_bones_(ctx, vp, id, w, col);
    else draw_origin_glyph_(ctx, vp, id, w, col);
}

void ViewportChromePanel::draw_pose_bones_(imm::Context& ctx, const ViewProj& vp, ObjectId id, const glm::mat4& w, const glm::vec4& col) {
    const glm::vec3 pos(w[3]);
    const glm::vec3 X = glm::normalize(glm::vec3(w[0])), Z = glm::normalize(glm::vec3(w[2]));
    // One octahedral bone reaching each child joint, so the rig reads as a skeleton.
    for (const glm::vec3& tail : app_.bone_tails_(id, app_.origin_size_for_(id) * 2.0f)) {
        glm::vec2 head, tip, q[4];
        if (!project_bone(vp, pos, tail, X, Z, head, tip, q)) continue;
        const glm::vec4 fill = imm::with_alpha(col, col.a * 0.25f);
        for (int i = 0; i < 4; ++i) {
            ctx.triangle(head, q[i], q[(i + 1) % 4], fill);
            ctx.triangle(tip, q[i], q[(i + 1) % 4], fill);
        }
        for (int i = 0; i < 4; ++i) {
            ctx.line(head, q[i], col, 1.3f);
            ctx.line(q[i], tip, col, 1.3f);
            ctx.line(q[i], q[(i + 1) % 4], col, 1.0f);
        }
    }
    // The joint, so a chain of bones reads as linked.
    if (const auto c = vp.project(pos)) ctx.ring(*c, 3.5f, 1.3f, col);
}

void ViewportChromePanel::draw_origin_glyph_(imm::Context& ctx, const ViewProj& vp, ObjectId id, const glm::mat4& w, const glm::vec4& col) {
    const glm::vec3 pos(w[3]);
    const glm::vec3 X = glm::normalize(glm::vec3(w[0])), Y = glm::normalize(glm::vec3(w[1])), Z = glm::normalize(glm::vec3(w[2]));
    const float a = std::max(0.001f, app_.origin_size_for_(id));
    auto P = [&](const glm::vec3& p) { return vp.project(p); };
    auto seg = [&](const glm::vec3& p, const glm::vec3& q, float t) {
        const auto pa = P(p), pb = P(q);
        if (pa && pb) ctx.line(*pa, *pb, col, t);
    };
    switch (app_.origin_style_for_(id)) {
    case OriginStyle::Orb: {
        // A ball at the origin with a short tick down each axis: the least cluttered display
        // for a dense rig, and still enough to read the object's orientation.
        const auto c = P(pos);
        if (!c) break;
        const float t = a * 0.45f;
        seg(pos, pos + X * t, 1.2f);
        seg(pos, pos + Y * t, 1.2f);
        seg(pos, pos + Z * t, 1.2f);
        ctx.circle(*c, 3.0f, col);
        ctx.ring(*c, 5.5f, 1.3f, col);
        break;
    }
    case OriginStyle::Axes:
    default:
        seg(pos - X * a, pos + X * a, 1.3f);
        seg(pos - Y * a, pos + Y * a, 1.3f);
        seg(pos - Z * a, pos + Z * a, 1.3f);
        break;
    }
}

float ViewportChromePanel::origin_glyph_pick_px_(ObjectId id, const ViewProj& vp, const glm::mat4& world, glm::vec2 px) {
    const glm::vec3 pos(world[3]);
    const auto centre = vp.project(pos);
    if (!centre) return 1e30f;
    float best = glm::distance(*centre, px);   // the origin point is clickable whatever is drawn
    // Only what is actually on screen is clickable by its shape; with the overlay off, or on
    // a camera / light (which draws its own symbol), the origin point is all there is.
    const bool pose_bone = app_.in_pose_mode_() && app_.timeline_panel_->in_anim_rig_(id) && app_.is_joint_object_(id);
    if (!app_.show_overlays_ || (!pose_bone && !app_.show_origins_) || !app_.is_joint_object_(id)) return best;

    const glm::vec3 X = glm::normalize(glm::vec3(world[0])), Y = glm::normalize(glm::vec3(world[1])),
                    Z = glm::normalize(glm::vec3(world[2]));
    const float a = std::max(0.001f, app_.origin_size_for_(id));
    auto segment = [&](const glm::vec3& p, const glm::vec3& q) {
        const auto pa = vp.project(p), pb = vp.project(q);
        if (pa && pb) best = std::min(best, point_segment_distance(px, *pa, *pb));
    };
    if (pose_bone) {
        for (const glm::vec3& tail : app_.bone_tails_(id, a * 2.0f)) {
            glm::vec2 head, tip, q[4];
            if (!project_bone(vp, pos, tail, X, Z, head, tip, q)) continue;
            for (int i = 0; i < 4; ++i) {
                const glm::vec2 b = q[i], c = q[(i + 1) % 4];
                // Anywhere on the body, not just its outline -- a bone is a solid to click.
                if (in_triangle(px, head, b, c) || in_triangle(px, tip, b, c)) return 0.0f;
                best = std::min({best, point_segment_distance(px, head, b), point_segment_distance(px, b, tip),
                                 point_segment_distance(px, b, c)});
            }
        }
        return best;
    }
    switch (app_.origin_style_for_(id)) {
    case OriginStyle::Orb:
        best = std::max(0.0f, best - 6.0f);   // the ball itself, then its axis ticks
        segment(pos, pos + X * (a * 0.45f));
        segment(pos, pos + Y * (a * 0.45f));
        segment(pos, pos + Z * (a * 0.45f));
        break;
    case OriginStyle::Axes:
    default:
        segment(pos - X * a, pos + X * a);
        segment(pos - Y * a, pos + Y * a);
        segment(pos - Z * a, pos + Z * a);
        break;
    }
    return best;
}

void ViewportChromePanel::draw_mode_menu_items_(imm::Context& ctx) {
    using I = imm::Icon;
    const bool mesh_obj = app_.active_type_ == AssetType::Mesh ||
                          (!app_.asset_view_() && (app_.edit_object_ || (app_.doc_.primary() && app_.doc_.find_component(app_.doc_.primary(), "MeshRenderer") >= 0)));
    const InteractionMode cur = app_.interaction_mode();
    bool o = cur == InteractionMode::Object, e = cur == InteractionMode::Edit, sc = cur == InteractionMode::Sculpt;
    bool vp = cur == InteractionMode::VertexPaint, wp = cur == InteractionMode::WeightPaint;
    const bool terrain = app_.terrain_panel_->in_terrain_mode_();
    const bool tiles = app_.tile_mode_panel_->in_tile_mode_();
    const bool painting_instances = app_.instance_paint_panel_->in_instance_mode_();
    o = o && !terrain && !tiles && !painting_instances;
    if (ctx.menu_item("Object Mode", "Tab", &o, true, I::ObjectMode)) {
        if (terrain) app_.terrain_panel_->exit_terrain_mode_();
        if (tiles) app_.tile_mode_panel_->exit_tile_mode_();
        if (painting_instances) app_.instance_paint_panel_->exit_instance_mode_();
        app_.set_interaction_mode(InteractionMode::Object);
    }
    if (ctx.menu_item("Edit Mode", "Tab", &e, mesh_obj && !app_.playing(), I::EditMode)) app_.set_interaction_mode(InteractionMode::Edit);
    if (ctx.menu_item("Sculpt Mode", "", &sc, mesh_obj && !app_.playing(), I::SculptMode)) app_.set_interaction_mode(InteractionMode::Sculpt);
    if (ctx.menu_item("Vertex Paint", "", &vp, mesh_obj && !app_.playing(), I::VertexPaint)) app_.set_interaction_mode(InteractionMode::VertexPaint);
    if (ctx.menu_item("Weight Paint", "", &wp, mesh_obj && !app_.playing(), I::WeightPaint)) app_.set_interaction_mode(InteractionMode::WeightPaint);
    // Any object: draw a Bezier path on it. Unlike the mesh modes this needs nothing of the
    // object first -- entering adds the Curve component when it has none, which is what "draw a
    // path here" means.
    bool pen = cur == InteractionMode::Pen;
    if (ctx.menu_item("Pen Mode", "", &pen, !app_.asset_view_() && app_.doc_.primary() && !app_.playing(),
                      I::Edge)) {
        if (pen) app_.curve_pen_panel_->end_pen_();
        else app_.curve_pen_panel_->begin_pen_(app_.doc_.primary());
    }
    ctx.tooltip("Pen Mode\nDraw and reshape a curve on this object. Click to add a point, click "
                "the line to insert one, drag a point or its handles, X deletes, C closes the loop.\n"
                "With the Select tool, click any other curve's line to switch to it -- which is "
                "how a hair groom's guides are moved between. On a Hair object the mode opens on "
                "a guide rather than on the scalp.");
    // A rig (an Animator at or above the selection): pose its joints by hand.
    bool pm = cur == InteractionMode::Pose;
    const ObjectId pose_id = app_.pose_rig_ ? app_.pose_rig_ : app_.timeline_panel_->rig_of_(app_.doc_.primary());
    if (ctx.menu_item("Pose Mode", "", &pm, !app_.asset_view_() && pose_id && !app_.playing(), I::ObjectMode)) {
        if (app_.pose_rig_) app_.exit_pose_mode_();
        else app_.enter_pose_mode_(pose_id);
    }
    ctx.tooltip("Pose Mode\nPose the rig's joints by hand: they draw as bones, a move edits the pose instead of the "
                "rest transform, and I keys it at the playhead (Auto-Key keys every edit)");
    // A HeightmapTerrain object: sculpt and paint the terrain itself.
    bool tm = terrain;
    const ObjectId terrain_id = terrain ? app_.terrain_panel_->terrain_object_ : app_.doc_.primary();
    if (ctx.menu_item("Terrain Mode", "", &tm, !app_.asset_view_() && app_.terrain_panel_->is_terrain_object_(terrain_id) && !app_.playing(), I::Terrain)) {
        if (terrain) app_.terrain_panel_->exit_terrain_mode_();
        else app_.terrain_panel_->enter_terrain_mode_(terrain_id);
    }
    // A TileTerrain object: edit its tiles.
    bool tl = tiles;
    const ObjectId tiles_id = tiles ? app_.tile_mode_panel_->tile_object_ : app_.doc_.primary();
    if (ctx.menu_item("Tile Mode", "", &tl, !app_.asset_view_() && app_.tile_mode_panel_->is_tile_terrain_object_(tiles_id) && !app_.playing(), I::Terrain)) {
        if (tiles) app_.tile_mode_panel_->exit_tile_mode_();
        else app_.tile_mode_panel_->enter_tile_mode_(tiles_id);
    }
    // An InstancePainter object: brush instances of its prototypes onto any surface in the scene.
    bool ip = painting_instances;
    const ObjectId painter_id = painting_instances ? app_.instance_paint_panel_->instance_object_ : app_.doc_.primary();
    if (ctx.menu_item("Instance Paint", "", &ip,
                      !app_.asset_view_() && app_.instance_paint_panel_->is_painter_object_(painter_id) && !app_.playing(),
                      I::BrushDraw)) {
        if (painting_instances) app_.instance_paint_panel_->exit_instance_mode_();
        else app_.instance_paint_panel_->enter_instance_mode_(painter_id);
    }
    ctx.tooltip("Instance Paint\nBrush instances of object assets onto the terrain and onto anything "
                "else in the scene. They are records, not objects: a world of them costs no scene file.");
}

void ViewportChromePanel::draw_shading_popover_(imm::Context& ctx) {
    auto& rc = app_.engine_.render_config();
    ctx.label_dim(app_.shading_ == Shading::Full ? "Rendered" : app_.shading_ == Shading::MaterialPreview ? "Material Preview"
                 : app_.shading_ == Shading::Solid ? "Solid" : "Wireframe");
    if (app_.shading_ == Shading::Solid || app_.shading_ == Shading::MaterialPreview) {
        const bool has_ssao = rc.ssao_enabled;
        bool ao = app_.viewport_ao_ && has_ssao;
        if (ctx.checkbox("Ambient Occlusion", &ao) && has_ssao) app_.set_viewport_ao(ao);
        ctx.tooltip(has_ssao ? "Ambient Occlusion\nThe renderer's SSAO, applied exactly as the full render applies it"
                             : "Ambient Occlusion\nUnavailable: the project's render config has ssao_enabled: false");
    }
    bool xr = app_.xray_;
    if (ctx.checkbox("X-Ray", &xr)) app_.xray_ = xr;
    ctx.tooltip("X-Ray\nSee and select through surfaces (Alt Z)");
    if (app_.xray_ && (app_.shading_ == Shading::Solid || app_.shading_ == Shading::MaterialPreview)) {
        ctx.slider_float("X-Ray Alpha", &app_.xray_alpha_, 0.0f, 1.0f);
        ctx.tooltip("X-Ray Alpha\nHow opaque surfaces stay in X-Ray (Blender's default 0.5)");
    }
    if (app_.shading_ != Shading::Full) { ctx.end_popup(); return; }
    ctx.separator();
    ctx.label_dim("Renderer (live, this session)");
    auto live = [&](const char* label, bool& v, const char* tip) {
        ctx.checkbox(label, &v);
        ctx.tooltip(tip);
    };
    live("Shadows", rc.shadows_enabled, "Shadows\nShadow maps for every light");
    live("Contact Shadows", rc.contact_shadows_enabled, "Contact Shadows\nScreen-space short shadows");
    live("Soft Lighting", rc.soft_lighting, "Soft Lighting\nSmooth lighting instead of the banded stylised look");
    live("Outline", rc.outline_enabled, "Outline\nThe stylised edge outline");
    live("Palette", rc.palette_enabled, "Palette\nPalette quantization");
    live("Dither", rc.dither_enabled, "Dither\nOrdered dithering");
    ctx.separator();
    ctx.label_dim("Set at startup (Properties > Render)");
    auto fixed = [&](const char* label, bool v) {
        bool copy = v;
        imm::Box row = ctx.next_box(ctx.style.row_height);
        ctx.text_in(row, std::string(copy ? "on    " : "off   ") + label, ctx.style.text_disabled, 0.0f);
        ctx.tooltip(std::string(label) + "\nChanged in Properties > Render; applies after Render > Restart Renderer");
    };
    fixed("SSAO", rc.ssao_enabled);
    fixed("SSR", rc.ssr_enabled);
    fixed("Bloom", rc.bloom_enabled);
    fixed("Fog", rc.fog_enabled);
    fixed("Volumetrics", rc.volumetrics_enabled);
    fixed("Depth of Field", rc.dof_enabled);
    ctx.end_popup();
}

void ViewportChromePanel::draw_colliders_popover_(imm::Context& ctx) {
    ctx.label_dim("Physics Colliders");
    ctx.checkbox("Show Colliders", &app_.show_colliders_);
    ctx.tooltip("Show Colliders\nWireframes of the physics colliders: green solid, yellow triggers");
    if (ctx.selectable("All Colliders", app_.collider_view_ == EditorApp::ColliderView::All)) app_.collider_view_ = EditorApp::ColliderView::All;
    ctx.tooltip("All Colliders\nEvery collider in the scene");
    if (ctx.selectable("Selected + Children", app_.collider_view_ == EditorApp::ColliderView::Selected)) app_.collider_view_ = EditorApp::ColliderView::Selected;
    ctx.tooltip("Selected + Children\nOnly the selected objects' colliders, and their children's");
    ctx.end_popup();
}

void ViewportChromePanel::draw_overlays_popover_(imm::Context& ctx) {
    ctx.label_dim("Viewport Overlays");
    ctx.checkbox("Grid", &app_.show_grid_);
    ctx.checkbox("Origins", &app_.show_origins_);
    ctx.tooltip("Origins\nThe origin glyph on every object that has no mesh of its own -- a rig's joints, markers, group parents");
    ctx.checkbox("Cameras / Lights", &app_.show_glyphs_);
    ctx.tooltip("Cameras / Lights\nCamera frustums and light symbols");
    ctx.checkbox("3D Cursor", &app_.show_cursor3d_);
    ctx.checkbox("Statistics", &app_.show_stats_overlay_);
    ctx.tooltip("Statistics\nObject / vertex / face counts in the viewport corner");
    // The viewer-wide default for origin glyphs, within reach of what it changes. Any object
    // can override it in Properties > Object > Origin Gizmo. Saved once the mouse lets go.
    ctx.spacing(4);
    ctx.label_dim("Object Origins (default)");
    ctx.slider_float("Size", &app_.origin_size_, 0.01f, 0.5f, "%.3f m");
    const bool size_done = ctx.last_deactivated();
    app_.test_rects_["origin_size"] = ctx.last_rect();
    ctx.tooltip("Size\nHow big an origin glyph is drawn by default, in metres. Turn it down for a dense rig");
    if (size_done) app_.preferences_panel_->prefs_save_();
    // Rows rather than a dropdown: a combo's popup would close this one.
    static const char* kOriginStyles[] = {"Axes", "Orb"};
    static const char* kOriginTips[] = {
        "Axes\nA line through the origin along each of the object's own axes",
        "Orb\nA small ball with a short tick down each axis -- the least cluttered on a dense rig",
    };
    for (int i = 0; i < 2; ++i) {
        ctx.push_id(i);
        if (ctx.selectable(kOriginStyles[i], app_.origin_style_ == i) && app_.origin_style_ != i) {
            app_.origin_style_ = i;
            app_.preferences_panel_->prefs_apply_();
            app_.preferences_panel_->prefs_save_();
        }
        app_.test_rects_[std::string("origin_style:") + kOriginStyles[i]] = ctx.last_rect();
        ctx.tooltip(kOriginTips[i]);
        ctx.pop_id();
    }
    ctx.end_popup();
}

} // namespace editor
} // namespace toy
