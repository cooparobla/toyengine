// editor/app/ui/mesh_tools.inl -- included inside EditorApp's class body.
//
// Blender's quad-modelling tools in the viewport:
//   - visibility-aware element picking (a BVH over the edited mesh: in Solid shading only
//     what you can see is selectable, unless X-ray is on);
//   - Alt+click edge loop, Ctrl+Alt+click ring (Shift toggles), deferred so that Alt+drag
//     still orbits;
//   - Loop Cut and Slide (Ctrl+R): a hover preview of the ring under the cursor, the wheel or
//     digits set the number of cuts, LMB cuts and slides (RMB leaves it centred);
//   - Edge Slide (G G);
//   - the topology operators (Subdivide, Triangulate, Tris to Quads, Dissolve, Bridge, Add
//     in Edit Mode) and the "Adjust Last Operation" panel that re-runs the last one.

    // =================================================================================
    // BVH, visibility and picking
    // =================================================================================

    const TriangleBVH& mesh_bvh_() {
        if (bvh_revision_ != mesh_.geometry_revision || !bvh_.built()) {
            bvh_.build(mesh_.mesh);
            bvh_revision_ = mesh_.geometry_revision;
        }
        return bvh_;
    }

    /** @brief Mesh-local ray through window point `px`. */
    void mesh_local_ray_(const ViewProj& vp, glm::vec2 px, glm::vec3& o, glm::vec3& d) {
        glm::vec3 ow, dw;
        vp.ray(px, ow, dw);
        const glm::mat4 inv = glm::inverse(mesh_world_());
        o = glm::vec3(inv * glm::vec4(ow, 1.0f));
        d = glm::normalize(glm::vec3(inv * glm::vec4(dw, 0.0f)));
    }

    /** @brief Does the surface hide this mesh-local point from the camera? */
    bool mesh_point_visible_(const ViewProj& vp, const glm::vec3& local_p) {
        if (xray_ || shading_ == Shading::Wireframe) return true;
        const glm::mat4 w = mesh_world_();
        const glm::vec3 pw = glm::vec3(w * glm::vec4(local_p, 1.0f));
        glm::vec3 eye_w;
        if (vp.ortho) eye_w = pw - vp.camera_forward() * 1e4f;
        else eye_w = glm::vec3(glm::inverse(vp.view)[3]);
        const glm::vec3 eye = glm::vec3(glm::inverse(w) * glm::vec4(eye_w, 1.0f));
        return !mesh_bvh_().occluded(mesh_.mesh, eye, local_p, 2e-3f);
    }

    /**
     * @brief Per-element visibility for the edit overlay, cached per (geometry, view).
     *        Large meshes (> 60k vertices) skip the test and draw everything.
     */
    struct EditVisibility {
        uint64_t revision = 0;
        glm::mat4 view{0.0f}, proj{0.0f};
        bool xray = false;
        std::vector<char> verts, faces;
        std::map<Edge, char> edges;
    };
    const EditVisibility& edit_visibility_(const ViewProj& vp) {
        const bool see_all = xray_ || shading_ == Shading::Wireframe || mesh_.mesh.positions.size() > 60000;
        EditVisibility& v = edit_vis_;
        if (v.revision == mesh_.geometry_revision && v.view == vp.view && v.proj == vp.proj && v.xray == see_all) return v;
        v.revision = mesh_.geometry_revision;
        v.view = vp.view;
        v.proj = vp.proj;
        v.xray = see_all;
        const auto& mm = mesh_.mesh;
        v.verts.assign(mm.positions.size(), 1);
        v.faces.assign(mm.faces.size(), 1);
        v.edges.clear();
        if (see_all) {
            for (const auto& e : mm.edges()) v.edges[e] = 1;
            return v;
        }
        for (uint32_t i = 0; i < mm.positions.size(); ++i) v.verts[i] = mesh_point_visible_(vp, mm.positions[i]);
        for (uint32_t f = 0; f < mm.faces.size(); ++f) v.faces[f] = mesh_point_visible_(vp, mm.face_center(f));
        for (const auto& e : mm.edges()) {
            v.edges[e] = (v.verts[e.first] && v.verts[e.second]) ||
                         mesh_point_visible_(vp, (mm.positions[e.first] + mm.positions[e.second]) * 0.5f);
        }
        return v;
    }

    /**
     * @brief The edge under the cursor, as Blender's Loop Cut and edge picking choose it: the
     *        nearest side (on screen) of the face under the cursor; failing a face hit, the
     *        nearest visible edge within 12 px.
     */
    std::optional<Edge> pick_edge_(const ViewProj& vp, glm::vec2 px) {
        const auto& mm = mesh_.mesh;
        if (mm.faces.empty()) return std::nullopt;
        const glm::mat4 w = mesh_world_();
        auto scr = [&](uint32_t v) { return vp.project(glm::vec3(w * glm::vec4(mm.positions[v], 1.0f))); };
        glm::vec3 o, d;
        mesh_local_ray_(vp, px, o, d);
        if (auto hit = mesh_bvh_().raycast(mm, o, d)) {
            const auto& c = mm.faces[hit->face].corners;
            std::optional<Edge> best;
            float bd = 1e30f;
            for (size_t i = 0; i < c.size(); ++i) {
                auto a = scr(c[i].v), b = scr(c[(i + 1) % c.size()].v);
                if (!a || !b) continue;
                const float dist = point_segment_distance(px, *a, *b);
                if (dist < bd) { bd = dist; best = make_edge(c[i].v, c[(i + 1) % c.size()].v); }
            }
            if (best) return best;
        }
        const EditVisibility& vis = edit_visibility_(vp);
        std::optional<Edge> best;
        float bd = 12.0f;
        for (const auto& [e, visible] : vis.edges) {
            if (!visible) continue;
            auto a = scr(e.first), b = scr(e.second);
            if (!a || !b) continue;
            const float dist = point_segment_distance(px, *a, *b);
            if (dist < bd) { bd = dist; best = e; }
        }
        return best;
    }

    /** @brief Click selection of one vertex / edge / face (visible ones, unless X-ray). */
    void pick_mesh_element_(glm::vec2 px, bool additive) {
        auto vp = view_proj_();
        if (!vp) return;
        auto& mm = mesh_.mesh;
        auto& sel = mesh_.selection;
        const glm::mat4 w = mesh_world_();
        auto wp = [&](uint32_t v) { return glm::vec3(w * glm::vec4(mm.positions[v], 1.0f)); };
        if (!additive) { sel.verts.clear(); sel.edges.clear(); sel.faces.clear(); }
        auto toggle = [&](auto& set, const auto& v) { if (!set.insert(v).second && additive) set.erase(v); };
        if (sel.mode == SelectMode::Vertex) {
            const EditVisibility& vis = edit_visibility_(*vp);
            int best = -1;
            float bd = 12.0f;
            for (uint32_t v = 0; v < mm.positions.size(); ++v) {
                if (!vis.verts[v]) continue;
                auto p = vp->project(wp(v));
                if (p && glm::distance(*p, px) < bd) { bd = glm::distance(*p, px); best = static_cast<int>(v); }
            }
            if (best >= 0) toggle(sel.verts, static_cast<uint32_t>(best));
        } else if (sel.mode == SelectMode::Edge) {
            // Nearest visible edge on screen (not the face-side rule: a click near an edge
            // should get that edge even if the cursor is just off the mesh).
            const EditVisibility& vis = edit_visibility_(*vp);
            std::optional<Edge> best;
            float bd = 10.0f;
            for (const auto& [e, visible] : vis.edges) {
                if (!visible) continue;
                auto a = vp->project(wp(e.first)), b = vp->project(wp(e.second));
                if (!a || !b) continue;
                const float d = point_segment_distance(px, *a, *b);
                if (d < bd) { bd = d; best = e; }
            }
            if (best) toggle(sel.edges, *best);
        } else {
            glm::vec3 o, d;
            mesh_local_ray_(*vp, px, o, d);
            if (auto hit = mesh_bvh_().raycast(mm, o, d)) toggle(sel.faces, hit->face);
        }
    }

    /** @brief Alt+click (loop) / Ctrl+Alt+click (ring) at `px`; Shift toggles. */
    void loop_select_at_(glm::vec2 px, bool ring, bool additive) {
        auto vp = view_proj_();
        if (!vp) return;
        const auto e = pick_edge_(*vp, px);
        if (!e) return;
        if (ring) select_edge_ring(mesh_.mesh, mesh_.selection, *e, additive);
        else select_edge_loop(mesh_.mesh, mesh_.selection, *e, additive);
    }

    // =================================================================================
    // Loop Cut and Slide, Edge Slide
    // =================================================================================

    struct LoopCutTool {
        bool active = false;
        int cuts = 1;
        std::optional<Edge> seed;
        uint64_t revision = 0;
        int preview_cuts = 0;
        std::vector<std::vector<glm::vec3>> lines;   ///< mesh-local polylines
    };

public:
    /** @brief Ctrl+R: starts Loop Cut and Slide (hover a ring, wheel for cuts, LMB to cut). */
    void begin_loop_cut() {
        if (!mesh_edit_view_() || playing()) return;
        loopcut_ = LoopCutTool{};
        loopcut_.active = true;
    }
    bool loop_cut_active() const { return loopcut_.active; }
    int loop_cut_cuts() const { return loopcut_.cuts; }

private:
    /** @brief One frame of the Loop Cut tool; owns the mouse (navigation still works). */
    void run_loop_cut_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
        using coopa::input::KeyAction;
        const auto& in = ctx.input();
        const glm::vec2 m = ctx.mouse();
        for (const auto& e : in.keys) {
            if (e.action == KeyAction::Release) continue;
            const int k = static_cast<int>(e.key);
            if (e.key == Key::Escape) { loopcut_.active = false; ctx.consume_keyboard(); return; }
            if (k >= static_cast<int>(Key::Num1) && k <= static_cast<int>(Key::Num9)) loopcut_.cuts = k - static_cast<int>(Key::Num0);
            if (k >= static_cast<int>(Key::Kp1) && k <= static_cast<int>(Key::Kp9)) loopcut_.cuts = k - static_cast<int>(Key::Kp0);
            if (e.key == Key::Equal || e.key == Key::KpAdd || e.key == Key::PageUp) ++loopcut_.cuts;
            if (e.key == Key::Minus || e.key == Key::KpSubtract || e.key == Key::PageDown) --loopcut_.cuts;
        }
        ctx.consume_keyboard();
        select_pending_ = box_selecting_ = false;
        if (in.pressed[1]) { loopcut_.active = false; return; }
        if (hovered && in.scroll.y != 0.0f) loopcut_.cuts += in.scroll.y > 0 ? 1 : -1;
        loopcut_.cuts = std::clamp(loopcut_.cuts, 1, 100);
        if (hovered) loopcut_.seed = pick_edge_(vp, m);
        if (loopcut_.seed && (loopcut_.revision != mesh_.geometry_revision || loopcut_.preview_cuts != loopcut_.cuts ||
                              loopcut_.lines.empty() || preview_seed_ != *loopcut_.seed)) {
            const MeshTopology t(mesh_.mesh);
            loopcut_.lines = loop_cut_preview(mesh_.mesh, t, t.find_edge(*loopcut_.seed), loopcut_.cuts);
            loopcut_.revision = mesh_.geometry_revision;
            loopcut_.preview_cuts = loopcut_.cuts;
            preview_seed_ = *loopcut_.seed;
        }
        if (!loopcut_.seed) loopcut_.lines.clear();
        if (hovered && in.pressed[0] && loopcut_.seed && !loopcut_.lines.empty()) {
            EditMesh trial = mesh_.mesh;
            MeshSelection trial_sel = mesh_.selection;
            const LoopCutResult res = loop_cut(trial, trial_sel, *loopcut_.seed, loopcut_.cuts);
            loopcut_.active = false;
            if (!res.ok) { log_warn("Loop Cut: " + res.error); return; }
            LastOp op;
            op.kind = LastOp::Kind::LoopCut;
            op.base = mesh_.mesh;
            op.base_sel = mesh_.selection;
            op.seed = *loopcut_.seed;
            op.cuts = loopcut_.cuts;
            mesh_.edit("Loop Cut and Slide", [&](EditMesh& mm, MeshSelection& s) { mm = std::move(trial); s = trial_sel; }, "loopcut");
            op.revision_after = mesh_.geometry_revision;
            last_op_ = op;
            if (loopcut_.cuts == 1 && !res.rails.empty()) start_edge_slide_(vp, m, res.rails, "loopcut");
            else mesh_.undo.end_merge();
        }
    }

    /** @brief Starts the Edge Slide modal for `rails` (mesh-local). */
    void start_edge_slide_(const ViewProj& vp, glm::vec2 m, const std::vector<SlideRail>& rails, const std::string& merge_key) {
        if (rails.empty()) return;
        const glm::mat4 w = mesh_world_();
        auto W = [&](const glm::vec3& p) { return glm::vec3(w * glm::vec4(p, 1.0f)); };
        // The rail nearest the mouse (that can move both ways) defines the slide direction.
        size_t best = 0;
        float bd = 1e30f;
        for (size_t i = 0; i < rails.size(); ++i) {
            if (glm::distance(rails[i].a, rails[i].b) < 1e-7f) continue;
            if (auto p = vp.project(W(rails[i].origin))) {
                const float d = glm::distance(*p, m);
                if (d < bd) { bd = d; best = i; }
            }
        }
        const SlideRail& r = rails[best];
        glm::vec3 dir = W(r.a) - W(r.b);
        if (glm::length(dir) < 1e-7f) dir = glm::vec3(1, 0, 0);
        begin_transform_(true);
        modal_mesh_ = true;
        modal_base_sel_ = mesh_.selection;
        slide_rails_ = rails;
        slide_merge_key_ = merge_key;
        modal_.begin(ModalKind::EdgeSlide, vp, W(r.origin), glm::mat3(1.0f), m, dir);
    }

    /** @brief G G: slide the selected edges (vertex mode: the edges between selected vertices). */
    void begin_edge_slide_from_selection_(const ViewProj& vp, glm::vec2 m) {
        std::set<Edge> es = mesh_.selection.edges;
        if (mesh_.selection.mode != SelectMode::Edge) {
            es.clear();
            const auto vs = mesh_.selection.affected_vertices(mesh_.mesh);
            for (const auto& e : mesh_.mesh.edges()) if (vs.count(e.first) && vs.count(e.second)) es.insert(e);
        }
        std::string err;
        const auto rails = edge_slide_rails(mesh_.mesh, es, &err);
        if (!rails) { log_warn("Edge Slide: " + err); return; }
        start_edge_slide_(vp, m, *rails, "modal");
    }

    /** @brief Loop Cut preview (yellow, like Blender) over the edit overlay. */
    void draw_mesh_tool_overlay_(imm::Context& ctx, const ViewProj& vp) {
        if (!loopcut_.active) return;
        const glm::mat4 w = mesh_world_();
        const glm::vec4 col(1.0f, 0.83f, 0.0f, 1.0f);
        for (const auto& line : loopcut_.lines) {
            for (size_t i = 0; i + 1 < line.size(); ++i) {
                auto a = vp.project(glm::vec3(w * glm::vec4(line[i], 1.0f)));
                auto b = vp.project(glm::vec3(w * glm::vec4(line[i + 1], 1.0f)));
                if (a && b) ctx.line(*a, *b, col, 2.0f);
            }
        }
        const float x = viewport_box_.x + (show_toolbar_ ? kToolSize + 22 : 10);
        shadow_text_(ctx, {x, viewport_box_.y + 56}, "Loop Cut and Slide  -  " + std::to_string(loopcut_.cuts) +
                                                       (loopcut_.cuts == 1 ? " cut" : " cuts"), ctx.style.object_active);
    }

    // =================================================================================
    // Topology operators + Adjust Last Operation
    // =================================================================================

    /** @brief What the "Adjust Last Operation" panel can re-run. */
    struct LastOp {
        enum class Kind { None, LoopCut, Subdivide, CatmullClark, AddMeshPrimitive, AddObjectPrimitive } kind = Kind::None;
        EditMesh base;
        MeshSelection base_sel;
        uint64_t revision_after = 0;
        Edge seed{0, 0};
        int cuts = 1;
        float factor = 0.0f;             ///< Loop cut: the edge slide applied after it.
        int levels = 1;
        PrimitiveParams prim;
        glm::mat4 prim_xf{1.0f};
        ObjectId object = 0;             ///< Object-mode Add: the object created.
        uint64_t doc_revision_after = 0;
    };

    /** @brief Records a mesh op for the panel (call right after its mesh_.edit). */
    void record_mesh_op_(LastOp op) {
        op.revision_after = mesh_.geometry_revision;
        last_op_ = std::move(op);
    }

    bool last_op_valid_() const {
        switch (last_op_.kind) {
            case LastOp::Kind::None: return false;
            case LastOp::Kind::AddObjectPrimitive:
                return !mesh_edit_view_() && doc_.find(last_op_.object) && doc_.undo_revision() == last_op_.doc_revision_after;
            default: return mesh_edit_view_() && mesh_.geometry_revision == last_op_.revision_after;
        }
    }

    /** @brief Runs a mesh operator as one undo step, recording it for the panel. */
    void run_subdivide_(int cuts) {
        LastOp op;
        op.kind = LastOp::Kind::Subdivide;
        op.base = mesh_.mesh;
        op.base_sel = mesh_.selection;
        op.cuts = cuts;
        if (mesh_.selection.affected_faces(mesh_.mesh).empty()) { log_warn("Subdivide: select faces (or all edges of faces)"); return; }
        mesh_.edit("Subdivide", [&](EditMesh& mm, MeshSelection& s) { subdivide(mm, s, cuts); });
        record_mesh_op_(op);
    }
    void run_catmull_clark_(int levels) {
        LastOp op;
        op.kind = LastOp::Kind::CatmullClark;
        op.base = mesh_.mesh;
        op.base_sel = mesh_.selection;
        op.levels = levels;
        mesh_.edit("Subdivide Smooth", [&](EditMesh& mm, MeshSelection& s) { catmull_clark(mm, s, levels); });
        record_mesh_op_(op);
    }
    /** @brief Shift+A in Edit Mode: a primitive added into the mesh at the 3D cursor. */
    void add_mesh_primitive_(const std::string& kind) {
        LastOp op;
        op.kind = LastOp::Kind::AddMeshPrimitive;
        op.base = mesh_.mesh;
        op.base_sel = mesh_.selection;
        op.prim = PrimitiveParams::defaults(kind);
        op.prim_xf = glm::inverse(mesh_world_()) * glm::translate(glm::mat4(1.0f), cursor3d_);
        mesh_.edit("Add " + kind, [&](EditMesh& mm, MeshSelection& s) {
            s.mode = SelectMode::Face;
            append_mesh(mm, make_primitive(op.prim), op.prim_xf, s);
        });
        record_mesh_op_(op);
    }

    /** @brief Re-runs the last operation with its edited parameters (replacing it in undo). */
    void rerun_last_op_() {
        LastOp& op = last_op_;
        if (op.kind == LastOp::Kind::AddObjectPrimitive) { rerun_object_primitive_(); return; }
        mesh_.do_undo();   // back to op.base (the op was one undo step)
        mesh_.mesh = op.base;
        const MeshSelection base_sel = op.base_sel;
        switch (op.kind) {
            case LastOp::Kind::LoopCut:
                mesh_.edit("Loop Cut and Slide", [&](EditMesh& mm, MeshSelection& s) {
                    s = base_sel;
                    loop_cut(mm, s, op.seed, op.cuts, op.cuts == 1 ? -op.factor : 0.0f);
                });
                break;
            case LastOp::Kind::Subdivide:
                mesh_.edit("Subdivide", [&](EditMesh& mm, MeshSelection& s) { s = base_sel; subdivide(mm, s, op.cuts); });
                break;
            case LastOp::Kind::CatmullClark:
                mesh_.edit("Subdivide Smooth", [&](EditMesh& mm, MeshSelection& s) { s = base_sel; catmull_clark(mm, s, op.levels); });
                break;
            case LastOp::Kind::AddMeshPrimitive:
                mesh_.edit("Add " + op.prim.kind, [&](EditMesh& mm, MeshSelection& s) {
                    s = base_sel;
                    s.mode = SelectMode::Face;
                    append_mesh(mm, make_primitive(op.prim), op.prim_xf, s);
                });
                break;
            default: break;
        }
        op.revision_after = mesh_.geometry_revision;
    }

    /**
     * @brief Object-mode Add with changed parameters: writes the mesh under a name made of
     *        its parameters (never touching a shared default mesh) and repoints the object.
     */
    void rerun_object_primitive_() {
        LastOp& op = last_op_;
        const PrimitiveParams& p = op.prim;
        std::string key = p.kind;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        for (char& c : key) if (c == ' ') c = '_';
        char buf[160];
        std::snprintf(buf, sizeof(buf), "_s%g_r%g_d%g_x%d_y%d_seg%d_ring%d%s", p.size, p.radius, p.depth, p.x_subdivisions,
                      p.y_subdivisions, p.segments, p.rings, p.caps ? "" : "_open");
        if (!(p == PrimitiveParams::defaults(p.kind))) key += buf;
        for (char& c : key) if (c == '.') c = 'p';
        const fs::path mesh_path = project_.assets() / "meshes" / (key + ".yaml");
        try {
            if (!coopa::yaml::document_exists(mesh_path)) {
                coopa::yaml::save_document(mesh_path, mesh_to_node(make_primitive(p)));
                project_.refresh();
            }
        } catch (const std::exception& e) {
            log_error(std::string("Could not write mesh: ") + e.what());
            return;
        }
        const int mr = doc_.find_component(op.object, "MeshRenderer");
        if (mr < 0) return;
        Node comp = doc_.find(op.object)->at("components").as_seq()[static_cast<size_t>(mr)];
        comp["mesh_path"] = Node(key);
        apply_(doc_.set_component(op.object, mr, comp, "Adjust " + p.kind, "lastop"));
        mesh_cache_.clear();
        op.doc_revision_after = doc_.undo_revision();
    }

    /** @brief Bottom-left panel: the last operation's parameters (Blender's F9 / redo panel). */
    void draw_last_op_panel_(imm::Context& ctx, const imm::Box& view) {
        last_op_rect_ = {};
        if (!last_op_valid_()) return;
        LastOp& op = last_op_;
        const char* title = op.kind == LastOp::Kind::LoopCut ? "Loop Cut and Slide"
                          : op.kind == LastOp::Kind::Subdivide ? "Subdivide"
                          : op.kind == LastOp::Kind::CatmullClark ? "Subdivide Smooth" : "Add Mesh";
        int rows = 1;
        if (last_op_open_) {
            switch (op.kind) {
                case LastOp::Kind::LoopCut: rows += op.cuts == 1 ? 2 : 1; break;
                case LastOp::Kind::Subdivide: case LastOp::Kind::CatmullClark: rows += 1; break;
                default: {
                    const std::string& k = op.prim.kind;
                    rows += k == "Grid" ? 3 : k == "Cylinder" ? 4 : k == "Sphere" ? 3 : 1;
                }
            }
        }
        const float rh = ctx.style.row_height + ctx.style.spacing;
        const float h = rows * rh + ctx.style.padding * 2 + 4;
        const imm::Box r{view.x + 10, view.bottom() - h - 10, 250, h};
        last_op_rect_ = r;
        ctx.shadow(r, 6);
        ctx.fill_rounded(r, imm::with_alpha(ctx.style.panel_bg, 0.96f), 6);
        ctx.begin_region("last_op_panel", r, false);
        const bool open = ctx.collapsing_header(title, true);
        last_op_open_ = open;
        bool changed = false;
        if (open) {
            auto drag_i = [&](const char* label, int* v, int lo, int hi) {
                if (ctx.drag_int(label, v, 0.1f, lo, hi)) changed = true;
            };
            auto drag_f = [&](const char* label, float* v, float lo, float hi, float speed) {
                if (ctx.drag_float(label, v, speed, lo, hi)) changed = true;
            };
            switch (op.kind) {
                case LastOp::Kind::LoopCut:
                    drag_i("Number of Cuts", &op.cuts, 1, 100);
                    if (op.cuts == 1) drag_f("Factor", &op.factor, -1.0f, 1.0f, 0.01f);
                    break;
                case LastOp::Kind::Subdivide: drag_i("Number of Cuts", &op.cuts, 1, 32); break;
                case LastOp::Kind::CatmullClark: drag_i("Levels", &op.levels, 1, 4); break;
                default: {
                    PrimitiveParams& p = op.prim;
                    if (p.kind == "Grid") {
                        drag_i("X Subdivisions", &p.x_subdivisions, 1, 500);
                        drag_i("Y Subdivisions", &p.y_subdivisions, 1, 500);
                        drag_f("Size", &p.size, 0.001f, 1e4f, 0.01f);
                    } else if (p.kind == "Cylinder") {
                        drag_i("Vertices", &p.segments, 3, 500);
                        drag_f("Radius", &p.radius, 0.001f, 1e4f, 0.01f);
                        drag_f("Depth", &p.depth, 0.001f, 1e4f, 0.01f);
                        if (ctx.checkbox("Cap Fill", &p.caps)) changed = true;
                    } else if (p.kind == "Sphere") {
                        drag_i("Segments", &p.segments, 3, 500);
                        drag_i("Rings", &p.rings, 2, 500);
                        drag_f("Radius", &p.radius, 0.001f, 1e4f, 0.01f);
                    } else {
                        drag_f("Size", &p.size, 0.001f, 1e4f, 0.01f);
                    }
                }
            }
        }
        ctx.end_region();
        // Re-run once the drag settles (each frame while dragging is fine for small meshes).
        if (changed) rerun_last_op_();
    }

    /** @brief A Loop Cut's slide ended: the panel's Factor, and its validity, follow it. */
    void note_slide_finished_(ModalKind kind, float factor) {
        if (kind != ModalKind::EdgeSlide || slide_merge_key_ != "loopcut" || last_op_.kind != LastOp::Kind::LoopCut) return;
        last_op_.factor = factor;
        last_op_.revision_after = mesh_.geometry_revision;
    }

    /**
     * @brief Mesh > Mirror: reflects the selection along axis `a` (the world's, or the mesh's
     *        own) through the selection's centre. Wholly selected faces keep facing outward.
     */
    void mirror_mesh_selection_(int a, bool global) {
        auto& md = mesh_;
        if (md.selection.affected_vertices(md.mesh).empty()) { log_warn("Mirror: nothing selected"); return; }
        const glm::vec3 pivot = selection_center(md.mesh, md.selection);
        const glm::mat4 reflect = mirror_plane_matrix(a, global, pivot, mesh_world_());
        static const char* kAxes[] = {"X", "Y", "Z"};
        md.edit(std::string("Mirror ") + kAxes[a], [&](EditMesh& mm, MeshSelection& s) { mirror_selection(mm, s, reflect); });
    }

    void bridge_selected_() {
        std::string err;
        bool ok = false;
        EditMesh trial = mesh_.mesh;
        MeshSelection s = mesh_.selection;
        ok = bridge_edge_loops(trial, s, &err);
        if (!ok) { log_warn("Bridge Edge Loops: " + err); return; }
        mesh_.edit("Bridge Edge Loops", [&](EditMesh& mm, MeshSelection& sel) { mm = std::move(trial); sel = s; });
    }

    void dissolve_selected_edges_() {
        std::set<Edge> es = mesh_.selection.edges;
        if (mesh_.selection.mode != SelectMode::Edge) {
            const auto vs = mesh_.selection.affected_vertices(mesh_.mesh);
            for (const auto& e : mesh_.mesh.edges()) if (vs.count(e.first) && vs.count(e.second)) es.insert(e);
        }
        size_t skipped = 0;
        mesh_.edit("Dissolve Edges", [&](EditMesh& mm, MeshSelection& s) { dissolve_edges(mm, s, es, true, &skipped); });
        if (skipped) log_warn("Dissolve: skipped " + std::to_string(skipped) + " region(s) that would leave a hole");
    }

    /** @brief Dissolve Faces: merges each connected group of selected faces into one. */
    void dissolve_selected_faces_() {
        const auto fs = mesh_.selection.affected_faces(mesh_.mesh);
        const MeshTopology t(mesh_.mesh);
        std::set<Edge> interior;
        for (uint32_t e = 0; e < t.edges.size(); ++e) {
            const auto adj = t.faces_of(e);
            if (adj.size() == 2 && fs.count(adj[0]) && fs.count(adj[1])) interior.insert(t.edges[e]);
        }
        size_t skipped = 0;
        mesh_.edit("Dissolve Faces", [&](EditMesh& mm, MeshSelection& s) { dissolve_edges(mm, s, interior, true, &skipped); });
        if (skipped) log_warn("Dissolve: skipped " + std::to_string(skipped) + " region(s) that would leave a hole");
    }
