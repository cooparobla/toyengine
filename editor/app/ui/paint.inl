// editor/app/ui/paint.inl -- included inside EditorApp's class body.
//
// Blender's Vertex Paint and Weight Paint Modes on the active mesh object. They are Sculpt
// Mode's siblings (ui/sculpt.inl) and share its machinery: the same SculptCache (adjacency,
// normals, BVH) and VertexGrid (radius queries) -- the modes are exclusive -- and strokes made
// of spaced dabs, one undo step per stroke (MeshDocument::begin_live / end_live).
//
// The brushes themselves are pure functions (mesh/paint.h). While painting, the mesh is drawn
// through a PaintPreview whose colours are rewritten as the stroke goes, with the editor-only
// `editor_paint` surface shader swapped onto the object's material (restored when the mode is
// left). Solid shading shows paint through Material Preview's studio look (apply_shading_()).
//
// Keys (Blender's): LMB paint, Ctrl inverts (secondary colour / subtract weight), Shift blurs,
// F / Shift+F radius / strength, X swaps the colours, S samples the colour or weight under the
// cursor, Shift+K fills the whole mesh, Tab back to Object Mode.

    // =================================================================================
    // Modes
    // =================================================================================

    static bool is_paint_mode_(InteractionMode m) { return m == InteractionMode::VertexPaint || m == InteractionMode::WeightPaint; }
    bool in_paint_mode_() const { return mesh_mode_target_() && is_paint_mode_(mode_); }
    bool in_weight_paint_() const { return mesh_mode_target_() && mode_ == InteractionMode::WeightPaint; }
    /** @brief Sculpt or a paint mode: brush strokes, no selection or transforms. */
    bool in_brush_mode_() const { return in_sculpt_mode_() || in_paint_mode_(); }

    /** @brief Starts / stops the per-mode machinery when the interaction mode changes. */
    void mode_begin_(InteractionMode m) {
        if (m == InteractionMode::Sculpt) sculpt_begin_();
        else if (is_paint_mode_(m)) paint_begin_();
    }
    void mode_end_(InteractionMode m) {
        if (m == InteractionMode::Sculpt) sculpt_end_();
        else if (is_paint_mode_(m)) paint_end_();
    }

    PaintSettings& paint_settings_() { return mode_ == InteractionMode::WeightPaint ? wpaint_ : vpaint_; }
    PaintPreview::Show paint_show_() const { return {mode_ == InteractionMode::WeightPaint, active_group_}; }

    // =================================================================================
    // Lifecycle
    // =================================================================================

    void paint_begin_() {
        if (mesh_.mesh.faces.empty()) return;
        if (active_group_ >= mesh_.mesh.groups.size()) active_group_ = 0;
        try {
            sculpt_cache_.build(mesh_.mesh);
            paint_preview_.build(engine_.device(), engine_.allocator(), mesh_.mesh, sculpt_cache_, paint_show_());
        } catch (const std::exception& e) {
            log_error(std::string(mode_ == InteractionMode::WeightPaint ? "Weight Paint: " : "Vertex Paint: ") + e.what());
            paint_active_ = false;
            return;
        }
        paint_active_ = true;
        paint_geom_ = mesh_.geometry_revision;
        paint_pos_ = mesh_.position_revision;
        paint_shown_ = paint_show_();
        assign_paint_preview_();
        apply_shading_();
        tool_ = Tool::Select;
    }

    void paint_end_() {
        if (paint_stroke_.active) end_paint_stroke_();
        paint_resize_.active = false;
        if (!paint_active_) return;
        paint_active_ = false;
        restore_paint_materials_();
        paint_preview_.reset();
        scene_uploaded_revision_ = 0;   // push_mesh_to_scene_ re-exports the painted mesh once
        uploaded_revision_ = 0;         // and the mesh viewer re-uploads it (with its own material)
        apply_shading_();
    }

    /** @brief Every live renderer showing the edited mesh (or the mesh viewer's), keyed by object (0 = viewer). */
    std::vector<std::pair<ObjectId, coopa::gfx::engine::components::MeshRenderer*>> edited_mesh_renderers_() {
        std::vector<std::pair<ObjectId, coopa::gfx::engine::components::MeshRenderer*>> out;
        if (active_type_ == AssetType::Mesh) {
            if (auto* mr = preview_renderer_()) out.push_back({0, mr});
            return out;
        }
        const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
        for (const auto& [id, live] : sync_.live_objects()) {
            const int ci = doc_.find_component(id, "MeshRenderer");
            if (ci < 0 || !live) continue;
            const std::string key = object_mesh_key_(*doc_.find(id));   // a skinned mesh's too
            const fs::path p = coopa::yaml::resolve_variant(engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
            if (p != mesh_.path) continue;
            if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) out.push_back({id, mr});
        }
        return out;
    }

    /** @brief Shows the paint preview (with the editor_paint shader) on every renderer of the mesh. */
    void assign_paint_preview_() {
        if (!paint_preview_.mesh()) return;
        auto handle = engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/paint_mesh", paint_preview_.mesh());
        for (auto& [id, mr] : edited_mesh_renderers_()) {
            if (!paint_saved_shader_.count(id)) paint_saved_shader_[id] = mr->material.shader;
            mr->material.shader = "editor_paint";
            mr->set_mesh(handle);
        }
        mesh_cache_.clear();
        scene_uploaded_revision_ = mesh_.geometry_revision;
    }

    /** @brief Puts each renderer's own surface shader back (where it still has the paint one). */
    void restore_paint_materials_() {
        for (auto& [id, mr] : edited_mesh_renderers_()) {
            auto it = paint_saved_shader_.find(id);
            if (it != paint_saved_shader_.end() && mr->material.shader == "editor_paint") mr->material.shader = it->second;
        }
        paint_saved_shader_.clear();
    }

    /** @brief Per frame (pre-render): follow undo / topology changes, upload painted colours. */
    void paint_frame_() {
        if (!paint_active_) return;
        if (!mesh_.live() && paint_geom_ != mesh_.geometry_revision) {
            const bool same_topology = SculptCache::topology_key(mesh_.mesh) == sculpt_cache_.topo_key;
            if (active_group_ >= mesh_.mesh.groups.size()) active_group_ = mesh_.mesh.groups.empty() ? 0u : static_cast<uint32_t>(mesh_.mesh.groups.size() - 1);
            try {
                sculpt_cache_.build(mesh_.mesh);
                if (!same_topology) {
                    paint_preview_.build(engine_.device(), engine_.allocator(), mesh_.mesh, sculpt_cache_, paint_show_());
                    assign_paint_preview_();
                }
            } catch (const std::exception& e) {
                log_error(std::string("Paint: ") + e.what());
            }
            paint_geom_ = mesh_.geometry_revision;
            paint_pos_ = 0;
        }
        scene_uploaded_revision_ = mesh_.geometry_revision;   // the preview shows it; no re-export
        const PaintPreview::Show show = paint_show_();
        if (paint_pos_ != mesh_.position_revision || show.weights != paint_shown_.weights || show.group != paint_shown_.group) {
            paint_preview_.update(mesh_.mesh, sculpt_cache_, show, engine_.frame_slot());
            paint_pos_ = mesh_.position_revision;
            paint_shown_ = show;
        }
    }

    // =================================================================================
    // Strokes
    // =================================================================================

    struct PaintStroke {
        bool active = false;
        PaintTool tool = PaintTool::Draw;
        bool invert = false;
        float radius = 0.1f;   ///< mesh-local
        glm::vec2 last_px{0.0f};
    };

    /** @brief A brush's screen radius `radius_px` as a mesh-local distance at `local_hit`. */
    float brush_local_radius_(const ViewProj& vp, const glm::vec3& local_hit, float radius_px) {
        const glm::mat4 w = mesh_world_();
        const glm::vec3 world = glm::vec3(w * glm::vec4(local_hit, 1.0f));
        const float scale = std::max({glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])), glm::length(glm::vec3(w[2])), 1e-6f});
        return radius_px * vp.world_per_pixel(world) / scale;
    }

    /** @brief The vertex group Weight Paint paints, creating "Group" if the mesh has none. */
    uint32_t paint_group_(EditMesh& m) {
        if (m.groups.empty()) {
            active_group_ = m.group_index("Group");
            log_info("Weight Paint: added vertex group \"Group\" to paint into");
        }
        return std::min<uint32_t>(active_group_, static_cast<uint32_t>(m.groups.size() - 1));
    }

    void paint_dab_at_(const ViewProj& vp, glm::vec2 px, std::vector<uint32_t>& changed) {
        glm::vec3 hit, n;
        if (!sculpt_hit_(vp, px, hit, n)) return;
        if (std::abs(sculpt_grid_.cell() - paint_stroke_.radius) > paint_stroke_.radius * 0.5f) sculpt_grid_.build(mesh_.mesh, paint_stroke_.radius);
        PaintSettings s = paint_settings_();
        s.tool = paint_stroke_.tool;
        PaintDab d;
        d.center = hit;
        d.view_dir = -n;
        d.radius = paint_stroke_.radius;
        d.strength = s.strength;
        d.invert = paint_stroke_.invert;
        SculptDab sd;   // symmetry is shared with Sculpt: mirror the dab's centre and view
        sd.center = d.center;
        sd.view_dir = d.view_dir;
        sd.radius = d.radius;
        const bool weights = mode_ == InteractionMode::WeightPaint;
        const uint32_t group = weights ? paint_group_(mesh_.mesh) : 0u;
        for_each_symmetric(sd, mirror_images(s.symmetry, mesh_world_()), [&](const SculptDab& md) {
            PaintDab pd = d;
            pd.center = md.center;
            pd.view_dir = md.view_dir;
            if (weights) weight_paint_dab(mesh_.mesh, sculpt_cache_, sculpt_grid_, s, group, pd, changed);
            else vertex_paint_dab(mesh_.mesh, sculpt_cache_, sculpt_grid_, s, pd, changed);
        });
    }

    void begin_paint_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl) {
        glm::vec3 hit, n;
        if (!sculpt_hit_(vp, px, hit, n)) return;
        const PaintSettings& s = paint_settings_();
        paint_stroke_ = PaintStroke{};
        paint_stroke_.active = true;
        paint_stroke_.tool = shift ? PaintTool::Blur : s.tool;
        paint_stroke_.invert = ctrl;
        paint_stroke_.radius = std::max(1e-5f, brush_local_radius_(vp, hit, s.radius_px));
        paint_stroke_.last_px = px;
        mesh_.begin_live();
        sculpt_grid_.build(mesh_.mesh, paint_stroke_.radius);
        std::vector<uint32_t> changed;
        paint_dab_at_(vp, px, changed);
        if (!changed.empty()) mesh_.touch_live();
    }

    void continue_paint_stroke_(const ViewProj& vp, glm::vec2 px) {
        const PaintSettings& s = paint_settings_();
        const float step = std::max(1.0f, s.spacing * s.radius_px);
        glm::vec2 cur = paint_stroke_.last_px;
        float dist = glm::distance(px, cur);
        std::vector<uint32_t> changed;
        int guard = 0;
        while (dist >= step && guard++ < 256) {
            cur += (px - cur) / dist * step;
            paint_dab_at_(vp, cur, changed);
            dist = glm::distance(px, cur);
        }
        paint_stroke_.last_px = cur;
        if (!changed.empty()) mesh_.touch_live();
    }

    void end_paint_stroke_() {
        if (!paint_stroke_.active) return;
        paint_stroke_.active = false;
        const bool weights = mode_ == InteractionMode::WeightPaint;
        mesh_.end_live(std::string(weights ? "Weight Paint " : "Vertex Paint ") + paint_tool_name(paint_stroke_.tool));
        paint_geom_ = mesh_.geometry_revision;   // no topology change: the cache is still right
    }

    /** @brief S: picks up the colour (or weight) of the vertex nearest the surface under `px`. */
    void paint_sample_(const ViewProj& vp, glm::vec2 px) {
        glm::vec3 o, d;
        mesh_local_ray_(vp, px, o, d);
        auto h = sculpt_cache_.bvh.raycast(mesh_.mesh, o, d);
        if (!h || h->face >= mesh_.mesh.faces.size()) return;
        const Face& f = mesh_.mesh.faces[h->face];
        const Corner* best = nullptr;
        float best_d = 1e30f;
        for (const auto& c : f.corners) {
            const float dd = glm::distance(mesh_.mesh.positions[c.v], h->p);
            if (dd < best_d) { best_d = dd; best = &c; }
        }
        if (!best) return;
        if (mode_ == InteractionMode::WeightPaint) {
            wpaint_.weight = mesh_.mesh.groups.empty() ? 0.0f : mesh_.mesh.weight(best->v, std::min<uint32_t>(active_group_, static_cast<uint32_t>(mesh_.mesh.groups.size() - 1)));
        } else {
            vpaint_.color = mesh_.mesh.has_colors ? best->color : glm::vec4(1.0f);
        }
    }

    /** @brief Shift+K / the menu's Fill: the brush colour on every corner, or the brush weight on every vertex. */
    void paint_fill_all_() {
        if (mode_ == InteractionMode::WeightPaint) {
            const float w = wpaint_.weight;
            mesh_.edit("Set Weight", [&](EditMesh& m, MeshSelection&) { assign_weight(m, paint_group_(m), w); });
        } else {
            const glm::vec4 c = vpaint_.color;
            mesh_.edit("Set Vertex Colors", [&](EditMesh& m, MeshSelection&) { fill_vertex_colors(m, c); });
        }
    }

    // =================================================================================
    // Input
    // =================================================================================

    /** @brief Paint Modes' mouse and keys (navigation is handled before this). */
    void handle_paint_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
        const auto& in = ctx.input();
        const glm::vec2 m = ctx.mouse();
        const bool shift = has(in.mods, Mods::Shift);
        const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
        if (!paint_active_) return;
        PaintSettings& s = paint_settings_();
        if (paint_resize_.active) {   // F / Shift+F, as in Sculpt Mode
            const float dx = m.x - paint_resize_.start.x;
            if (paint_resize_.strength) s.strength = std::clamp(paint_resize_.start_value + dx / 300.0f, 0.0f, 1.0f);
            else s.radius_px = std::clamp(paint_resize_.start_value + dx, 2.0f, 600.0f);
            bool cancel = in.pressed[1], confirm = in.pressed[0];
            for (const auto& e : in.keys) {
                if (e.action == coopa::input::KeyAction::Release) continue;
                if (e.key == Key::Escape) cancel = true;
                if (e.key == Key::Enter || e.key == Key::KpEnter) confirm = true;
            }
            ctx.consume_keyboard();
            if (cancel) {
                (paint_resize_.strength ? s.strength : s.radius_px) = paint_resize_.start_value;
                paint_resize_.active = false;
            } else if (confirm) {
                paint_resize_.active = false;
            }
            return;
        }
        if (paint_stroke_.active) {
            if (!in.down[0]) { end_paint_stroke_(); return; }
            if (in.mouse_delta != glm::vec2(0.0f)) continue_paint_stroke_(vp, m);
            return;
        }
        if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt)) begin_paint_stroke_(vp, m, shift, ctrl);
        if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
        viewport_keymap_(ctx, vp, false, false);   // views, shading, sidebar... (no object edits)
        if (ctx.shortcut(Key::F)) paint_resize_ = {true, false, m - glm::vec2(s.radius_px, 0.0f), s.radius_px};
        if (ctx.shortcut(Key::F, Mods::Shift)) paint_resize_ = {true, true, m - glm::vec2(s.strength * 300.0f, 0.0f), s.strength};
        if (ctx.shortcut(Key::X) && mode_ == InteractionMode::VertexPaint) std::swap(vpaint_.color, vpaint_.secondary);
        if (ctx.shortcut(Key::S)) paint_sample_(vp, m);
        if (ctx.shortcut(Key::K, Mods::Shift)) paint_fill_all_();
        if (ctx.shortcut(Key::Tab)) exit_mesh_mode_();
    }

    /** @brief The brush circle and the F / Shift+F gauge. */
    void draw_paint_overlay_(imm::Context& ctx, const ViewProj& vp) {
        if (!paint_active_) return;
        const PaintSettings& s = paint_settings_();
        draw_brush_overlay_(ctx, vp, s.radius_px, s.strength, paint_resize_, paint_stroke_.active, paint_stroke_.active && paint_stroke_.invert);
    }

    // =================================================================================
    // UI
    // =================================================================================

    /** @brief Header widgets in a paint mode: colour or weight, radius, strength. Returns the width used. */
    float draw_paint_header_(imm::Context& ctx, float x, const imm::Box& hb) {
        const float bh = hb.h - 6;
        PaintSettings& s = paint_settings_();
        auto label = [&](const char* t) {
            const float w = ctx.text_width(t) + 6;
            ctx.text_in({x, hb.y, w, hb.h}, t, ctx.style.text_dim, 0.0f);
            x += w;
        };
        if (mode_ == InteractionMode::VertexPaint) {
            // Primary and secondary swatches (X swaps): click opens the colour picker.
            auto swatch = [&](const char* id, glm::vec4& c, const char* tip) {
                const imm::Box b{x, hb.y + 3, bh * 1.4f, bh};
                bool hov = false, held = false;
                const std::string pop = std::string(id) + "_pop";
                if (ctx.invisible_button(id, b, &hov, &held)) ctx.open_popup(pop, glm::vec2(b.x, b.bottom() + 2));
                ctx.fill_rounded(b, glm::vec4(glm::vec3(c), 1.0f));
                if (hov) ctx.fill_rounded(b, glm::vec4(1, 1, 1, 0.12f));
                ctx.tooltip(tip);
                if (ctx.begin_popup(pop, 230)) {
                    ctx.color_edit("Color", &c.x, true);
                    ctx.end_popup();
                }
                x += b.w + 3;
            };
            swatch("vp_color", s.color, "Color\nThe colour the brush paints (S samples it from the mesh)");
            swatch("vp_secondary", s.secondary, "Secondary Color\nCtrl paints with this one; X swaps the two");
            x += 6;
        } else {
            label("Weight");
            ctx.drag_float_box("wp_weight", {x, hb.y + 3, 52, bh}, &s.weight, 0.005f, 0.0f, 1.0f, "%.2f");
            ctx.tooltip("Weight\nThe weight the brush lays down in the active vertex group (S samples it)");
            x += 58;
        }
        label("Radius");
        ctx.drag_float_box("pt_radius", {x, hb.y + 3, 62, bh}, &s.radius_px, 0.5f, 2.0f, 600.0f, "%.0f px");
        ctx.tooltip("Radius\nBrush radius in screen pixels (F)");
        x += 68;
        label("Strength");
        ctx.drag_float_box("pt_strength", {x, hb.y + 3, 56, bh}, &s.strength, 0.005f, 0.0f, 1.0f, "%.2f");
        ctx.tooltip("Strength\nHow strongly each dab acts (Shift F)");
        x += 64;
        return x + 8;
    }

    /** @brief The tool column of the toolbar (paint modes). */
    void draw_paint_toolbar_(imm::Context& ctx, const imm::Box& r) {
        using I = imm::Icon;
        float y = r.y + 4;
        PaintSettings& s = paint_settings_();
        struct B { PaintTool t; I ic; const char* tip; };
        static const B kTools[] = {
            {PaintTool::Draw, I::BrushDraw, "Draw\nPaint with the brush colour / weight (Ctrl inverts)"},
            {PaintTool::Blur, I::BrushSmooth, "Blur\nBlend each vertex with its neighbours; hold Shift with any tool"},
            {PaintTool::Average, I::BrushFlatten, "Average\nPull everything under the brush toward its average"},
        };
        for (const auto& b : kTools) {
            const imm::Box bb{r.x + 3, y, kToolSize, kToolSize};
            y += kToolSize + 2;
            if (ctx.icon_button(std::string("ptool_") + paint_tool_name(b.t), b.ic, b.tip, s.tool == b.t, kToolSize,
                                imm::Context::kAll, bb)) {
                s.tool = b.t;
            }
        }
    }

    /** @brief The header's "Paint" / "Weights" menu: whole-mesh operations. */
    void draw_paint_menu_(imm::Context& ctx) {
        using I = imm::Icon;
        if (mode_ == InteractionMode::VertexPaint) {
            if (!ctx.begin_menu("Paint")) return;
            if (ctx.menu_item("Set Vertex Colors", "Shift K", nullptr, true, I::VertexPaint)) paint_fill_all_();
            if (ctx.menu_item("Invert", "", nullptr, mesh_.mesh.has_colors)) mesh_.edit("Invert Colors", [](EditMesh& m, MeshSelection&) { invert_vertex_colors(m); });
            if (ctx.menu_item("Remove Color Attribute", "", nullptr, mesh_.mesh.has_colors, I::Trash)) {
                mesh_.edit("Remove Colors", [](EditMesh& m, MeshSelection&) { clear_vertex_colors(m); });
            }
            ctx.end_menu();
            return;
        }
        if (!ctx.begin_menu("Weights")) return;
        const bool any = !mesh_.mesh.groups.empty();
        if (ctx.menu_item("Set Weight", "Shift K", nullptr, true, I::WeightPaint)) paint_fill_all_();
        if (ctx.menu_item("Normalize All", "", nullptr, any)) mesh_.edit("Normalize All", [](EditMesh& m, MeshSelection&) { normalize_all_weights(m); });
        if (ctx.menu_item("Clear Group", "", nullptr, any, I::Trash)) {
            const uint32_t g = active_group_;
            mesh_.edit("Clear Group Weights", [g](EditMesh& m, MeshSelection&) { assign_weight(m, g, 0.0f); });
        }
        ctx.end_menu();
    }

    /**
     * @brief Properties > Tool > Vertex Groups (Weight Paint and Edit Mode): the group list,
     *        add / remove / rename, and in Edit Mode Blender's Assign / Remove / Select / Deselect
     *        on the selected vertices.
     */
    void draw_vertex_groups_panel_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!ctx.collapsing_header("Vertex Groups", true, nullptr, I::WeightPaint)) return;
        EditMesh& m = mesh_.mesh;
        if (m.groups.empty()) ctx.label_dim("No vertex groups.");
        for (uint32_t g = 0; g < m.groups.size(); ++g) {
            if (ctx.selectable(m.groups[g] + "##vg" + std::to_string(g), g == active_group_, 0.0f, I::WeightPaint)) active_group_ = g;
        }
        if (ctx.button("Add", -1, true, I::Plus)) {
            std::string name = "Group";
            for (int i = 1; std::find(m.groups.begin(), m.groups.end(), name) != m.groups.end(); ++i) name = "Group." + std::to_string(i);
            uint32_t added = 0;
            mesh_.edit("Add Vertex Group", [&](EditMesh& mm, MeshSelection&) { added = mm.group_index(name); });
            active_group_ = added;
        }
        if (!m.groups.empty()) {
            if (active_group_ >= m.groups.size()) active_group_ = static_cast<uint32_t>(m.groups.size() - 1);
            if (ctx.button("Remove", -1, true, I::Trash)) {
                const uint32_t g = active_group_;
                mesh_.edit("Remove Vertex Group", [g](EditMesh& mm, MeshSelection&) { mm.remove_group(g); });
                if (active_group_ > 0) --active_group_;
            }
            if (active_group_ < m.groups.size()) {
                std::string name = m.groups[active_group_];
                if (ctx.input_text("Name", &name) && !name.empty() &&
                    std::find(m.groups.begin(), m.groups.end(), name) == m.groups.end()) {
                    const uint32_t g = active_group_;
                    mesh_.edit("Rename Vertex Group", [g, name](EditMesh& mm, MeshSelection&) { mm.groups[g] = name; }, "rename_vg");
                }
            }
        }
        if (in_edit_mode_() && active_group_ < m.groups.size()) {
            ctx.slider_float("Weight", &edit_assign_weight_, 0.0f, 1.0f, "%.2f");
            const auto sel = mesh_.selection.affected_vertices(m);
            const std::vector<uint32_t> verts(sel.begin(), sel.end());
            const uint32_t g = active_group_;
            const float w = edit_assign_weight_;
            if (ctx.button("Assign", -1, !verts.empty())) mesh_.edit("Assign to Vertex Group", [&](EditMesh& mm, MeshSelection&) { assign_weight(mm, g, w, verts); });
            ctx.tooltip("Assign\nGive the selected vertices this weight in the active group");
            if (ctx.button("Remove from Group", -1, !verts.empty())) mesh_.edit("Remove from Vertex Group", [&](EditMesh& mm, MeshSelection&) { assign_weight(mm, g, 0.0f, verts); });
            if (ctx.button("Select", -1)) {
                for (uint32_t v = 0; v < m.positions.size(); ++v) if (m.weight(v, g) > 0.0f) mesh_.selection.verts.insert(v);
                mesh_.selection.mode = SelectMode::Vertex;
                mesh_.selection.validate(m);
            }
            ctx.tooltip("Select\nAdd the active group's vertices to the selection");
        }
    }

    /** @brief Properties > Tool in a paint mode: brush, symmetry, and the mode's data. */
    void draw_paint_tool_panel_(imm::Context& ctx) {
        using I = imm::Icon;
        PaintSettings& s = paint_settings_();
        const bool weights = mode_ == InteractionMode::WeightPaint;
        if (ctx.collapsing_header("Brush", true, nullptr, weights ? I::WeightPaint : I::VertexPaint)) {
            int t = static_cast<int>(s.tool);
            if (ctx.combo("Tool", &t, {"Draw", "Blur", "Average"})) s.tool = static_cast<PaintTool>(t);
            int b = static_cast<int>(s.blend);
            if (weights) {
                if (b > 2) b = 0;
                if (ctx.combo("Blend", &b, {"Mix", "Add", "Subtract"})) s.blend = static_cast<PaintBlend>(b);
                ctx.slider_float("Weight", &s.weight, 0.0f, 1.0f, "%.2f");
            } else {
                if (ctx.combo("Blend", &b, {"Mix", "Add", "Subtract", "Multiply", "Lighten", "Darken"})) s.blend = static_cast<PaintBlend>(b);
                ctx.color_edit("Color", &s.color.x, true);
                ctx.color_edit("Secondary", &s.secondary.x, true);
            }
            ctx.drag_float("Radius (px)", &s.radius_px, 0.5f, 2.0f, 600.0f);
            ctx.slider_float("Strength", &s.strength, 0.0f, 1.0f, "%.2f");
            ctx.slider_float("Spacing", &s.spacing, 0.02f, 1.0f, "%.2f");
            ctx.checkbox("Front Faces Only", &s.front_faces_only);
            ctx.tooltip("Front Faces Only\nSkip vertices facing away from the view, so a stroke does not bleed through to the far side");
            if (weights) {
                ctx.checkbox("Auto Normalize", &s.auto_normalize);
                ctx.tooltip("Auto Normalize\nRescale each painted vertex's other groups so its weights sum to 1");
            }
            draw_symmetry_panel_(ctx, s.symmetry);
        }
        if (weights) {
            draw_vertex_groups_panel_(ctx);
        } else if (ctx.collapsing_header("Color Attribute", true, nullptr, I::VertexPaint)) {
            ctx.label_dim(mesh_.mesh.has_colors ? "The mesh has vertex colours (saved as `colors`)." : "No colours yet: the first stroke adds them (white).");
            if (ctx.button("Fill with Color", -1, true, I::VertexPaint)) paint_fill_all_();
            if (ctx.button("Remove Colors", -1, mesh_.mesh.has_colors, I::Trash)) {
                mesh_.edit("Remove Colors", [](EditMesh& m, MeshSelection&) { clear_vertex_colors(m); });
            }
        }
    }

public:
    PaintSettings& vertex_paint_settings() { return vpaint_; }
    PaintSettings& weight_paint_settings() { return wpaint_; }
    uint32_t active_vertex_group() const { return active_group_; }
    void set_active_vertex_group(uint32_t g) { active_group_ = g; }
    bool paint_stroking() const { return paint_stroke_.active; }
    /** @brief The surface shader of the live renderer showing the edited mesh (tests). */
    std::string edited_mesh_shader() {
        auto rs = edited_mesh_renderers_();
        return rs.empty() ? std::string() : rs.front().second->material.shader;
    }

private:
