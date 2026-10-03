// editor/app/ui/viewport_chrome.inl -- included inside EditorApp's class body.
//
// The 3D Viewport's Blender furniture: the header (mode menu, View / Select / Add / Object
// menus, orientation, snapping, overlays, the four shading buttons), the toolbar column,
// the navigation gizmo, the N sidebar, the view-name overlay text, and Blender-style
// glyphs for lights, cameras and empties.

    // --- layout of the overlay furniture (computed before input so clicks on it never
    //     reach the viewport's select / navigate handlers) ---

    static constexpr float kToolSize = 32.0f;

    imm::Box toolbar_rect_(bool mesh_edit) const {
        const int n = in_sculpt_mode_() ? 5 : mesh_edit ? 9 : 5;
        return {viewport_box_.x + 6, viewport_box_.y + 6, kToolSize + 6, n * (kToolSize + 2) + 14};
    }
    float sidebar_w_() const { return show_sidebar_ ? 230.0f : 0.0f; }
    imm::Box sidebar_rect_() const { return {viewport_box_.right() - sidebar_w_(), viewport_box_.y, sidebar_w_(), viewport_box_.h}; }
    imm::Box nav_gizmo_rect_() const {
        return {viewport_box_.right() - sidebar_w_() - 118, viewport_box_.y + 8, 110, 110 + 4 * 30};
    }
    /** @brief True if `p` is over viewport furniture (toolbar, nav gizmo, sidebar). */
    bool over_viewport_chrome_(glm::vec2 p, bool mesh_edit) const {
        if (show_toolbar_ && toolbar_rect_(mesh_edit).contains(p)) return true;
        const imm::Box g = nav_gizmo_rect_();
        if (glm::distance(p, glm::vec2(g.x + 55, g.y + 55)) < 52.0f) return true;
        if (imm::Box{g.x + 40, g.y + 112, 30, 4 * 30}.contains(p)) return true;
        if (show_sidebar_ && sidebar_rect_().contains(p)) return true;
        if (last_op_rect_.contains(p)) return true;
        return false;
    }

    // --- header ---

    void draw_viewport_header_(imm::Context& ctx, const imm::Box& hb, bool mesh_edit) {
        using I = imm::Icon;
        float x = hb.x + 6;
        const float bh = hb.h - 6;
        if (playing()) ctx.fill_rounded(hb, et_.chrome.play_tint, 6, imm::Context::kTop);   // Unity's play tint
        // Mode dropdown.
        if (!asset_view_() || mesh_edit) {
            const bool sculpting = in_sculpt_mode_();
            const std::string mode = sculpting ? "Sculpt Mode" : mesh_edit ? "Edit Mode" : "Object Mode";
            const float w = ctx.text_width(mode) + bh + 26;
            const imm::Box mb{x, hb.y + 3, w, bh};
            bool hov = false, held = false;
            if (ctx.invisible_button("mode_dd", mb, &hov, &held)) ctx.open_popup("mode_menu", glm::vec2(mb.x, mb.bottom() + 2));
            ctx.fill_rounded(mb, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.icon(sculpting ? I::SculptMode : mesh_edit ? I::EditMode : I::ObjectMode, {mb.x + 4, mb.y + 2, bh - 4, bh - 4}, ctx.style.text);
            ctx.text_in({mb.x + bh + 2, mb.y, w - bh - 16, bh}, mode, ctx.style.text, 0.0f);
            ctx.arrow({mb.right() - 14, mb.y + 4, 10, bh - 8}, true, ctx.style.text_dim);
            ctx.tooltip("Mode\nObject Mode / Edit Mode (Tab) / Sculpt Mode (Ctrl Tab for the menu)");
            x += w + 8;
            if (ctx.begin_popup("mode_menu", 170)) {
                draw_mode_menu_items_(ctx);
                ctx.end_popup();
            }
            // Edit / Sculpt Mode isolation (Blender's Local View, entered automatically).
            if (edit_object_ && !asset_view_()) {
                if (ctx.icon_button("isolate", I::LocalView, "Isolate in Edit Mode\nEdit and Sculpt Mode hide the other objects and frame "
                                    "this one (lights stay). Saved as a preference.", isolate_in_edit_, bh, imm::Context::kAll,
                                    imm::Box{x, hb.y + 3, bh, bh}, true)) {
                    set_isolate_in_edit(!isolate_in_edit_);
                }
                x += bh + 8;
            }
            if (sculpting) x = draw_sculpt_header_(ctx, x, hb);
        }
        if (mesh_edit) {
            int sm = static_cast<int>(mesh_.selection.mode);
            if (ctx.icon_group("selmode", {{I::Vertex, "Vertex Select\n1"}, {I::Edge, "Edge Select\n2"}, {I::Face, "Face Select\n3"}}, &sm,
                               imm::Box{x, hb.y + 3, bh * 3, bh}, bh)) {
                if (static_cast<SelectMode>(sm) != mesh_.selection.mode) convert_selection(mesh_.mesh, mesh_.selection, static_cast<SelectMode>(sm));
            }
            x += bh * 3 + 8;
        }
        // Menus.
        const float menus_w = 220;
        ctx.begin_menubar({x, hb.y, menus_w, hb.h}, false);
        draw_view_menu_(ctx);
        if (!in_sculpt_mode_()) {
            draw_select_menu_(ctx, mesh_edit);
            if (!mesh_edit && ctx.begin_menu("Add")) { draw_add_menu_items_(ctx); ctx.end_menu(); }
            if (mesh_edit) draw_mesh_menu_(ctx);
            else draw_object_menu_(ctx);
        }
        ctx.end_menubar();

        // Right side: orientation, snap, overlays, x-ray, shading.
        float rx = hb.right() - 6;
        const float s = bh;
        // Blender's Viewport Shading popover, right of the four buttons.
        const imm::Box sdd{rx - s * 0.7f, hb.y + 3, s * 0.7f, s};
        if (ctx.icon_button("shading_opts_btn", I::ArrowDown, "Viewport Shading\nAmbient occlusion and other display options", false,
                            s * 0.7f, imm::Context::kAll, sdd)) {
            ctx.open_popup("shading_opts", glm::vec2(sdd.right() - 220, sdd.bottom() + 2));
        }
        if (ctx.begin_popup("shading_opts", 220)) {
            ctx.label_dim("Viewport Shading");
            const bool has_ssao = engine_.render_config().ssao_enabled;
            bool ao = viewport_ao_ && has_ssao;
            if (ctx.checkbox("Ambient Occlusion", &ao) && has_ssao) set_viewport_ao(ao);
            ctx.tooltip(has_ssao ? "Ambient Occlusion\nDarken creases with the renderer's SSAO in Solid and Material Preview"
                                 : "Ambient Occlusion\nUnavailable: the project's render config has ssao_enabled: false");
            bool xr = xray_;
            if (ctx.checkbox("X-Ray", &xr)) xray_ = xr;
            ctx.end_popup();
        }
        rx -= s * 0.7f + 2;
        rx -= s * 4;
        int sh = static_cast<int>(shading_);
        if (ctx.icon_group("shading", {{I::ShadeWire, "Wireframe\nShift Z toggles, Z for the menu"},
                                       {I::ShadeSolid, "Solid\nLighting-independent shading for modelling"},
                                       {I::ShadeMaterial, "Material Preview\nUnlit material colours and textures"},
                                       {I::ShadeRendered, "Rendered\nThe game's full renderer"}},
                           &sh, imm::Box{rx, hb.y + 3, s * 4, s}, s)) {
            set_shading(static_cast<Shading>(sh));
        }
        rx -= s + 10;
        if (ctx.icon_button("xray", I::XRay, "Toggle X-Ray\nShow wireframes of every object through surfaces", xray_, s, imm::Context::kAll,
                            imm::Box{rx, hb.y + 3, s, s}, true)) {
            xray_ = !xray_;
        }
        rx -= s + 4;
        if (ctx.icon_button("overlays", I::Overlays, "Overlays\nGrid, origins, light and camera glyphs", show_overlays_, s, imm::Context::kAll,
                            imm::Box{rx, hb.y + 3, s, s}, true)) {
            show_overlays_ = !show_overlays_;
        }
        rx -= s + 10;
        if (ctx.icon_button("snap", I::Snap, "Snap\nSnap transforms to increments (Ctrl inverts while dragging)", snap_on_, s, imm::Context::kAll,
                            imm::Box{rx, hb.y + 3, s, s}, true)) {
            snap_on_ = !snap_on_;
        }
        rx -= 92;
        const imm::Box ob{rx, hb.y + 3, 88, s};
        bool oh = false, oheld = false;
        if (ctx.invisible_button("orient_dd", ob, &oh, &oheld)) ctx.open_popup("orient_menu", glm::vec2(ob.x, ob.bottom() + 2));
        ctx.fill_rounded(ob, oh ? ctx.style.button_hover : ctx.style.button);
        ctx.icon(I::Orientation, {ob.x + 3, ob.y + 2, s - 4, s - 4}, ctx.style.text);
        static const char* kOrientNames[] = {"Global", "Local", "Normal"};
        ctx.text_in({ob.x + s, ob.y, ob.w - s - 12, s}, kOrientNames[static_cast<int>(orient_)], ctx.style.text, 0.0f);
        ctx.icon(I::ArrowDown, {ob.right() - 13, ob.y + 5, 9, s - 10}, ctx.style.text_dim);
        ctx.tooltip("Transform Orientation\nAxes the gizmo and a second X / Y / Z press use: Global, the object's Local axes, "
                    "or the selection's Normal (edit mode)");
        if (ctx.begin_popup("orient_menu", 150)) {
            for (int i = 0; i < 3; ++i) {
                bool on = static_cast<int>(orient_) == i;
                if (ctx.menu_item(kOrientNames[i], "", &on, i != 2 || mesh_edit)) orient_ = static_cast<Orientation>(i);
            }
            ctx.end_popup();
        }
    }

    void draw_view_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("View")) return;
        bool t = show_toolbar_, n = show_sidebar_;
        if (ctx.menu_item("Toolbar", "T", &t)) show_toolbar_ = !show_toolbar_;
        if (ctx.menu_item("Sidebar", "N", &n)) show_sidebar_ = !show_sidebar_;
        ctx.menu_separator();
        if (ctx.menu_item("Frame Selected", "Num .", nullptr, true, I::Zoom)) frame_selected();
        if (ctx.menu_item("Frame All", "Home", nullptr, true, I::Zoom)) frame_all();
        if (ctx.menu_item(camera_.ortho ? "Perspective" : "Orthographic", "Num 5", nullptr, true, camera_.ortho ? I::Persp : I::Ortho)) {
            camera_.ortho = !camera_.ortho;
            camera_.apply();
        }
        if (ctx.begin_menu("Viewpoint", true, I::ViewCamera)) {
            if (ctx.menu_item("Camera", "Num 0", nullptr, true, I::Camera)) view_through_scene_camera_();
            if (ctx.menu_item("Top", "Num 7")) camera_.axis_view('t', false);
            if (ctx.menu_item("Bottom", "Ctrl Num 7")) camera_.axis_view('t', true);
            if (ctx.menu_item("Front", "Num 1")) camera_.axis_view('f', false);
            if (ctx.menu_item("Back", "Ctrl Num 1")) camera_.axis_view('f', true);
            if (ctx.menu_item("Right", "Num 3")) camera_.axis_view('r', false);
            if (ctx.menu_item("Left", "Ctrl Num 3")) camera_.axis_view('r', true);
            ctx.end_menu();
        }
        if (ctx.menu_item("Align Active Camera to View", "Ctrl Alt Num 0", nullptr, !asset_view_(), I::Camera)) align_scene_camera_to_view_();
        ctx.menu_separator();
        if (ctx.menu_item("Toggle Maximize Area", "Ctrl Space")) maximized_ = !maximized_;
        ctx.end_menu();
    }

    void draw_select_menu_(imm::Context& ctx, bool mesh_edit) {
        using I = imm::Icon;
        if (!ctx.begin_menu("Select")) return;
        if (ctx.menu_item("All", "A")) {
            if (mesh_edit) mesh_.selection.select_all(mesh_.mesh);
            else { doc_.clear_selection(); for (ObjectId i : visible_ids_()) doc_.select(i, true); }
        }
        if (ctx.menu_item("None", "Alt A")) { if (mesh_edit) mesh_.selection.clear(); else doc_.clear_selection(); }
        if (ctx.menu_item("Invert", "Ctrl I")) {
            if (mesh_edit) invert_mesh_selection_();
            else {
                std::vector<ObjectId> inv;
                for (ObjectId i : visible_ids_()) if (!doc_.is_selected(i)) inv.push_back(i);
                doc_.clear_selection();
                for (ObjectId i : inv) doc_.select(i, true);
            }
        }
        ctx.menu_separator();
        if (ctx.menu_item("Box Select", "B", nullptr, true, I::SelectBox)) tool_ = Tool::Select;
        if (mesh_edit && ctx.menu_item("Select Linked", "Ctrl L", nullptr, true, I::Link)) select_linked(mesh_.mesh, mesh_.selection);
        ctx.end_menu();
    }

    void draw_add_menu_items_(imm::Context& ctx) {
        using I = imm::Icon;
        static const I prim_icons[] = {I::Cube, I::Plane, I::Cylinder, I::Sphere, I::Plane};
        if (ctx.begin_menu("Mesh", true, I::Mesh)) {
            const auto& names = primitive_names();
            for (size_t i = 0; i < names.size(); ++i) if (ctx.menu_item(names[i], "", nullptr, true, prim_icons[i % 5])) create_primitive(names[i]);
            ctx.end_menu();
        }
        if (ctx.begin_menu("Light", true, I::PointLight)) {
            if (ctx.menu_item("Sun", "", nullptr, true, I::Sun)) create_with_component("DirectionalLight", "Sun");
            if (ctx.menu_item("Point", "", nullptr, true, I::PointLight)) create_with_component("PointLight", "Point Light");
            if (ctx.menu_item("Spot", "", nullptr, true, I::SpotLight)) create_with_component("SpotLight", "Spot Light");
            if (ctx.menu_item("Environment", "", nullptr, true, I::EnvLight)) create_with_component("EnvironmentLight", "Environment");
            ctx.end_menu();
        }
        if (ctx.menu_item("Camera", "", nullptr, true, I::Camera)) create_with_component("Camera", "Camera");
        if (ctx.menu_item("Empty", "", nullptr, true, I::Empty)) create_empty();
        ctx.menu_separator();
        if (ctx.menu_item("Reflection Probe", "", nullptr, true, I::EnvLight)) create_with_component("ReflectionProbe", "Reflection Probe");
        if (ctx.menu_item("Terrain", "", nullptr, true, I::Terrain)) create_with_component("Terrain", "Terrain");
    }

    void draw_object_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("Object")) return;
        const bool any = !doc_.selection().empty() && !playing() && !asset_view_();
        if (ctx.begin_menu("Transform", any, I::Move)) {
            if (ctx.menu_item("Move", "G", nullptr, true, I::Move)) pending_modal_kind_ = ModalKind::Grab;
            if (ctx.menu_item("Rotate", "R", nullptr, true, I::Rotate)) pending_modal_kind_ = ModalKind::Rotate;
            if (ctx.menu_item("Scale", "S", nullptr, true, I::Scale)) pending_modal_kind_ = ModalKind::Scale;
            ctx.end_menu();
        }
        if (ctx.begin_menu("Clear", any)) {
            if (ctx.menu_item("Location", "Alt G")) clear_transform_(0);
            if (ctx.menu_item("Rotation", "Alt R")) clear_transform_(1);
            if (ctx.menu_item("Scale", "Alt S")) clear_transform_(2);
            ctx.end_menu();
        }
        if (ctx.begin_menu("Snap", true, I::Snap)) {
            if (ctx.menu_item("Cursor to Selected")) { glm::vec3 p; glm::mat3 b; if (gizmo_target_(false, p, b)) cursor3d_ = p; }
            if (ctx.menu_item("Cursor to World Origin")) cursor3d_ = glm::vec3(0.0f);
            if (ctx.menu_item("Selection to Cursor", "", nullptr, any)) selection_to_cursor_();
            ctx.end_menu();
        }
        if (ctx.begin_menu("Parent", any, I::Link)) {
            if (ctx.menu_item("Object (keep transform)", "Ctrl P", nullptr, doc_.selection().size() > 1)) parent_selection_to_active_();
            if (ctx.menu_item("Clear Parent (keep transform)", "Alt P")) clear_parent_keep_transform_();
            ctx.end_menu();
        }
        ctx.menu_separator();
        if (ctx.menu_item("Duplicate Objects", "Shift D", nullptr, any, I::Duplicate)) { duplicate_selected(); pending_modal_kind_ = ModalKind::Grab; }
        if (ctx.menu_item("Delete", "X", nullptr, any, I::Trash)) delete_selected();
        ctx.menu_separator();
        if (ctx.menu_item("Hide Selected", "H", nullptr, any, I::EyeClosed)) hide_(doc_.selection());
        if (ctx.menu_item("Show Hidden", "Alt H", nullptr, true, I::Eye)) unhide_all_();
        ctx.end_menu();
    }

    void draw_mesh_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.begin_menu("Mesh")) return;
        auto op = [&](const char* label, auto&& fn) { mesh_.edit(label, fn); };
        if (ctx.menu_item("Extrude", "E", nullptr, true, I::Extrude)) pending_extrude_ = true;
        if (ctx.menu_item("Inset Faces", "I", nullptr, true, I::Inset)) pending_modal_kind_ = ModalKind::Inset;
        if (ctx.menu_item("Bevel Edges", "Ctrl B", nullptr, true, I::Bevel)) pending_modal_kind_ = ModalKind::Bevel;
        if (ctx.menu_item("Make Face", "F", nullptr, true, I::Face)) op("Make Face", [](EditMesh& m, MeshSelection& s) { fill_face(m, s); });
        if (ctx.menu_item("Duplicate", "Shift D", nullptr, true, I::Duplicate)) {
            op("Duplicate", [](EditMesh& m, MeshSelection& s) { duplicate_faces(m, s); });
            pending_modal_kind_ = ModalKind::Grab;
        }
        ctx.menu_separator();
        if (ctx.begin_menu("Merge", true)) {
            if (ctx.menu_item("At Center", "M")) op("Merge", [](EditMesh& m, MeshSelection& s) { merge_at_center(m, s); });
            if (ctx.menu_item("By Distance")) op("Merge by Distance", [this](EditMesh& m, MeshSelection& s) { merge_by_distance(m, s, merge_dist_); });
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
            if (ctx.menu_item("Cube Projection", "U")) op("Box UV", [this](EditMesh& m, MeshSelection& s) { uv_box_project(m, s, uv_scale_); });
            if (ctx.menu_item("Project X")) op("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 0); });
            if (ctx.menu_item("Project Y")) op("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 1); });
            if (ctx.menu_item("Project Z")) op("Planar UV", [](EditMesh& m, MeshSelection& s) { uv_planar_project(m, s, 2); });
            ctx.end_menu();
        }
        ctx.end_menu();
    }

    // --- toolbar (T) ---

    void draw_toolbar_(imm::Context& ctx, bool mesh_edit) {
        using I = imm::Icon;
        const imm::Box r = toolbar_rect_(mesh_edit);
        ctx.fill_rounded(r, et_.viewport.toolbar_bg, 6);
        float y = r.y + 4;
        auto tool_btn = [&](const char* id, I ic, const char* tip, bool on) {
            const imm::Box b{r.x + 3, y, kToolSize, kToolSize};
            y += kToolSize + 2;
            return ctx.icon_button(id, ic, tip, on, kToolSize, imm::Context::kAll, b);
        };
        if (in_sculpt_mode_()) { draw_sculpt_toolbar_(ctx, r); return; }
        if (tool_btn("t_select", I::SelectBox, "Select Box\nClick or drag to select (Shift adds, Ctrl removes)", tool_ == Tool::Select)) tool_ = Tool::Select;
        if (tool_btn("t_cursor", I::Cursor, "Cursor\nClick to place the 3D cursor (Shift RMB anywhere)", tool_ == Tool::Cursor)) tool_ = Tool::Cursor;
        y += 6;
        if (tool_btn("t_move", I::Move, "Move\nGizmo for moving (G for the modal move)", tool_ == Tool::Move)) tool_ = Tool::Move;
        if (tool_btn("t_rotate", I::Rotate, "Rotate\nGizmo for rotating (R for the modal rotate)", tool_ == Tool::Rotate)) tool_ = Tool::Rotate;
        if (tool_btn("t_scale", I::Scale, "Scale\nGizmo for scaling (S for the modal scale)", tool_ == Tool::Scale)) tool_ = Tool::Scale;
        if (mesh_edit) {
            y += 6;
            if (tool_btn("t_extrude", I::Extrude, "Extrude Region\nExtrude the selection, then move it (E)", false)) pending_extrude_ = true;
            if (tool_btn("t_inset", I::Inset, "Inset Faces\nInset the selected faces; move the mouse to size (I)", false)) pending_modal_kind_ = ModalKind::Inset;
            if (tool_btn("t_bevel", I::Bevel, "Bevel\nBevel the selected edges; move the mouse to size (Ctrl B)", false)) pending_modal_kind_ = ModalKind::Bevel;
            if (tool_btn("t_loopcut", I::LoopCut, "Loop Cut and Slide\nHover an edge ring, wheel for more cuts, click to cut and slide (Ctrl R)",
                         loopcut_.active)) {
                begin_loop_cut();
            }
        }
    }

    // --- navigation gizmo ---

    void draw_nav_gizmo_(imm::Context& ctx) {
        using I = imm::Icon;
        const imm::Box g = nav_gizmo_rect_();
        const glm::vec2 c{g.x + 55, g.y + 55};
        const float R = 40.0f;
        const glm::mat3 vr = glm::mat3(view_proj_() ? view_proj_()->view : glm::mat4(1.0f));
        // Orbit by dragging the ball's background.
        bool hov = false, held = false;
        ctx.invisible_button("navball", {c.x - 50, c.y - 50, 100, 100}, &hov, &held);
        if (held && ctx.input().mouse_delta != glm::vec2(0.0f)) camera_.orbit(ctx.input().mouse_delta);
        if (hov || held) ctx.circle(c, 50, et_.viewport.nav_hover);
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
                ctx.text_in({b.p.x - 8, b.p.y - 8, 16, 16}, names[b.idx], et_.viewport.nav_label, 0, true);
            } else {
                ctx.circle(b.p, over ? 9.0f : 7.5f, imm::with_alpha(col, over ? 0.7f : 0.35f));
                ctx.ring(b.p, 7.5f, 1.2f, imm::with_alpha(col, 0.8f));
            }
            if (over && ctx.input().released[0] && !nav_ball_dragged_) { clicked = b.idx; clicked_pos = b.pos; }
        }
        if (held && glm::length(ctx.input().mouse_delta) > 0.0f) nav_ball_dragged_ = true;
        if (!ctx.input().down[0]) nav_ball_dragged_ = false;
        if (clicked >= 0) {
            // A ball looks FROM that axis, as in Blender.
            if (clicked == 0) camera_.axis_view('r', !clicked_pos);
            else if (clicked == 1) camera_.axis_view('f', clicked_pos);
            else camera_.axis_view('t', !clicked_pos);
        }
        // Zoom / pan drag buttons, camera and projection toggles.
        float y = g.y + 114;
        const float s = 26;
        const imm::Box zb{g.x + 42, y, s, s}, pb{g.x + 42, y + 30, s, s}, cb{g.x + 42, y + 60, s, s}, ob{g.x + 42, y + 90, s, s};
        ctx.fill_rounded({zb.x - 2, zb.y - 2, s + 4, s * 4 + 4 * 3 + 4}, et_.viewport.nav_backdrop, (s + 4) * 0.5f);
        bool zh = false, zheld = false, ph = false, pheld = false;
        ctx.invisible_button("nav_zoom", zb, &zh, &zheld);
        if (zh || zheld) ctx.circle(zb.center(), s * 0.5f, et_.viewport.nav_hover);
        ctx.icon(I::Zoom, zb.shrink(5), ctx.style.text);
        ctx.tooltip("Zoom\nDrag up and down to zoom the view");
        if (zheld) camera_.dolly(-ctx.input().mouse_delta.y * 0.04f);
        ctx.invisible_button("nav_pan", pb, &ph, &pheld);
        if (ph || pheld) ctx.circle(pb.center(), s * 0.5f, et_.viewport.nav_hover);
        ctx.icon(I::Hand, pb.shrink(5), ctx.style.text);
        ctx.tooltip("Move\nDrag to pan the view");
        if (pheld) camera_.pan(ctx.input().mouse_delta, viewport_box_.h);
        if (ctx.icon_button("nav_cam", I::Camera, "Toggle Camera View\nLook through the scene camera (Numpad 0)", false, s, imm::Context::kAll, cb)) {
            view_through_scene_camera_();
        }
        if (ctx.icon_button("nav_ortho", camera_.ortho ? I::Ortho : I::Persp, "Switch Projection\nPerspective / orthographic (Numpad 5)", false, s,
                            imm::Context::kAll, ob)) {
            camera_.ortho = !camera_.ortho;
            camera_.apply();
        }
    }

    // --- N sidebar ---

    void draw_sidebar_(imm::Context& ctx, bool mesh_edit) {
        using I = imm::Icon;
        const imm::Box r = sidebar_rect_();
        ctx.fill_rounded(r, et_.viewport.sidebar_bg, 6, imm::Context::kLeft);
        static const std::vector<std::string> tabs = {"Item", "View"};
        ctx.tab_bar("sidebar_tabs", {r.x, r.y, r.w, 26}, tabs, &sidebar_tab_, nullptr, false);
        ctx.begin_region("sidebar", {r.x, r.y + 26, r.w, r.h - 26}, true);
        const float saved_ratio = ctx.style.label_ratio;
        ctx.style.label_ratio = 0.34f;
        if (sidebar_tab_ == 0) {
            if (mesh_edit) {
                if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
                    const auto vs = mesh_.selection.affected_vertices(mesh_.mesh);
                    if (vs.empty()) ctx.label_dim("Nothing selected");
                    else {
                        glm::vec3 c = selection_center(mesh_.mesh, mesh_.selection);
                        glm::vec3 nc = c;
                        if (ctx.drag_float_stacked("Median", &nc.x, 3, 0.01f, "%.3f m")) {
                            const glm::vec3 d = nc - c;
                            mesh_.edit("Move Vertices", [&](EditMesh& m, MeshSelection& s) { translate_selection(m, s, d); }, "sidebar_median");
                        }
                        if (ctx.last_deactivated()) mesh_.undo.end_merge();
                    }
                }
            } else if (const ObjectId id = doc_.primary(); id && doc_.find(id)) {
                if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
                    glm::vec3 p, rr, s;
                    doc_.get_transform(id, p, rr, s);
                    glm::vec3 np = p, nr = rr, ns = s;
                    bool act = false, fin = false;
                    const bool a = ctx.drag_float_stacked("Location", &np.x, 3, 0.02f, "%.3f m"); act |= ctx.last_group_active(); fin |= ctx.last_deactivated();
                    const bool b = ctx.drag_float_stacked("Rotation", &nr.x, 3, 0.5f, "%.1f"); act |= ctx.last_group_active(); fin |= ctx.last_deactivated();
                    const bool cc = ctx.drag_float_stacked("Scale", &ns.x, 3, 0.01f, "%.3f"); act |= ctx.last_group_active(); fin |= ctx.last_deactivated();
                    if ((a || b || cc) && !playing()) apply_(doc_.set_transform(id, np, nr, ns, "Transform", act ? "sidebar_transform" : std::string()));
                    if (fin) doc_.end_merge();
                    glm::vec3 lo(1e30f), hi(-1e30f);
                    if (object_bounds_(id, lo, hi, true)) {
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
                if (ctx.drag_float("Field of View", &camera_.fov, 0.2f, 5.0f, 150.0f, "%.1f")) camera_.apply();
                if (ctx.drag_float("Distance", &camera_.distance, 0.05f, 0.05f, 5000.0f, "%.2f m")) camera_.apply();
                bool ortho = camera_.ortho;
                if (ctx.property_bool("Orthographic", &ortho)) { camera_.ortho = ortho; camera_.apply(); }
            }
            if (ctx.collapsing_header("3D Cursor", true, nullptr, I::Cursor)) {
                ctx.drag_float_stacked("Location", &cursor3d_.x, 3, 0.02f, "%.3f m");
            }
        }
        ctx.style.label_ratio = saved_ratio;
        ctx.end_region();
    }

    // --- overlay text and object glyphs ---

    std::string view_name_() const {
        const bool o = camera_.ortho;
        const float p = camera_.pitch_deg;
        const float y = std::fmod(std::fmod(camera_.yaw_deg, 360.0f) + 360.0f, 360.0f);
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

    /** @brief Viewport text with Blender's soft drop shadow, legible over any render. */
    void shadow_text_(imm::Context& ctx, glm::vec2 p, const std::string& s, const glm::vec4& c) const {
        ctx.draw_text(p + glm::vec2(1, 1), s, imm::with_alpha(et_.viewport.text_shadow, et_.viewport.text_shadow.a * c.a));
        ctx.draw_text(p, s, c);
    }

    void draw_overlay_text_(imm::Context& ctx, const ViewProj& vp) {
        const float x = viewport_box_.x + (show_toolbar_ ? kToolSize + 22 : 10);
        const float y0 = viewport_box_.y + 8;
        shadow_text_(ctx, {x, y0}, view_name_(), et_.viewport.overlay_text);
        (void)vp;
        std::string ctxline = "(1) " + doc_.scene_name();
        if (asset_view_()) ctxline = asset_kind_ == AssetKind::Mesh ? "(Mesh) " + mesh_.name : "(Material) " + material_.ref;
        else if (doc_.primary() && doc_.find(doc_.primary())) ctxline += " | " + get_string(*doc_.find(doc_.primary()), "name");
        shadow_text_(ctx, {x, y0 + 16}, ctxline, imm::with_alpha(et_.viewport.overlay_text, et_.viewport.overlay_text.a * 0.9f));
        if (playing()) shadow_text_(ctx, {x, y0 + 32}, play_scene_ && !play_scene_->is_simulating() ? "PAUSED" : "PLAYING",
                                    ctx.style.object_active);
    }

    /** @brief Blender's glyphs for a non-mesh object: camera frustum, light symbols, empty axes. */
    void draw_object_glyph_(imm::Context& ctx, const ViewProj& vp, const Node& node, const glm::mat4& w, const glm::vec4& col) {
        auto P = [&](const glm::vec3& p) { return vp.project(p); };
        auto seg = [&](const glm::vec3& a, const glm::vec3& b) {
            auto pa = P(a), pb = P(b);
            if (pa && pb) ctx.line(*pa, *pb, col, 1.3f);
        };
        const glm::vec3 pos(w[3]);
        const glm::vec3 X = glm::normalize(glm::vec3(w[0])), Y = glm::normalize(glm::vec3(w[1])), Z = glm::normalize(glm::vec3(w[2]));
        std::string kind;
        const Node* comp = nullptr;
        for (const auto& c : node.at("components").as_seq()) {
            const std::string t = component_type(c);
            if (t == "Camera" || t == "DirectionalLight" || t == "PointLight" || t == "SpotLight") { kind = t; comp = &c; break; }
        }
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
        // Empty: plain axes, half a unit each way.
        const float a = 0.5f;
        seg(pos - X * a, pos + X * a);
        seg(pos - Y * a, pos + Y * a);
        seg(pos - Z * a, pos + Z * a);
        (void)wpp;
    }

    /** @brief Object / Edit / Sculpt Mode entries (header dropdown, Ctrl+Tab, viewport RMB in Sculpt). */
    void draw_mode_menu_items_(imm::Context& ctx) {
        using I = imm::Icon;
        const bool mesh_obj = !asset_view_() && (edit_object_ || (doc_.primary() && doc_.find_component(doc_.primary(), "MeshRenderer") >= 0));
        const InteractionMode cur = interaction_mode();
        bool o = cur == InteractionMode::Object, e = cur == InteractionMode::Edit, sc = cur == InteractionMode::Sculpt;
        if (asset_view_()) e = mesh_edit_view_();
        if (ctx.menu_item("Object Mode", "Tab", &o, !asset_view_(), I::ObjectMode)) set_interaction_mode(InteractionMode::Object);
        if (ctx.menu_item("Edit Mode", "Tab", &e, mesh_obj && !playing(), I::EditMode)) set_interaction_mode(InteractionMode::Edit);
        if (ctx.menu_item("Sculpt Mode", "", &sc, mesh_obj && !playing(), I::SculptMode)) set_interaction_mode(InteractionMode::Sculpt);
    }
