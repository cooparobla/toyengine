// editor/app/ui/properties.inl -- included inside EditorApp's class body.
//
// Blender's Properties editor: a vertical strip of icon tabs, global ones first (Tool,
// Render, Output, Scene, World), then those of the active object (Object, Components --
// Unity's component list --, Physics, Object Data, Material). Render / Output / World edit
// config.yaml live, exactly where Blender keeps its render and output settings.

    void draw_properties_(imm::Context& ctx, const imm::Box& area) {
        using I = imm::Icon;
        properties_hovered_ = ctx.is_hovered(area);
        const ObjectId id = doc_.primary();
        const Node* obj = id ? doc_.find(id) : nullptr;
        const bool is_mesh = obj && doc_.find_component(id, "MeshRenderer") >= 0;

        struct TabDef { PropTab tab; I icon; const char* tip; };
        std::vector<TabDef> tabs = {
            {PropTab::Tool, I::Tool, "Tool\nActive tool and workspace settings"},
            {PropTab::Render, I::Render, "Render\nRenderer settings (config.yaml render)"},
            {PropTab::Output, I::Output, "Output\nWindow, jobs, packaging (config.yaml)"},
            {PropTab::Scene, I::Scene, "Scene\nScene settings, physics, startup scene"},
            {PropTab::World, I::World, "World\nSky, ambient light and fog"},
        };
        const int object_start = static_cast<int>(tabs.size());
        if (obj) {
            tabs.push_back({PropTab::Object, I::Object, "Object\nName, transform, relations, visibility"});
            tabs.push_back({PropTab::Components, I::Component, "Components\nThe object's components (Unity-style), Add Component"});
            tabs.push_back({PropTab::Physics, I::Physics, "Physics\nRigidbody, colliders, joints"});
            if (is_mesh) {
                tabs.push_back({PropTab::Data, I::Mesh, "Object Data\nMesh asset, LODs, statistics"});
                tabs.push_back({PropTab::Material, I::Material, "Material\nThe mesh's material: inline, asset or asset + overrides"});
            }
        }
        if (tab_ == Tab::Shading) prop_tab_ = PropTab::Material;
        int active = -1;
        for (size_t i = 0; i < tabs.size(); ++i) if (tabs[i].tab == prop_tab_) active = static_cast<int>(i);
        if (active < 0) { active = obj ? object_start : 0; prop_tab_ = tabs[static_cast<size_t>(active)].tab; }

        const float strip_w = 30;
        std::vector<std::pair<I, std::string>> items;
        for (const auto& t : tabs) items.push_back({t.icon, t.tip});
        const imm::Box strip{area.x, area.y + 4, strip_w, area.h - 8};
        if (tab_ == Tab::Shading) {
            // Shading pins the Material tab; show just that one, as Blender's Shading workspace does.
            int only = 0;
            ctx.vertical_tabs("prop_tabs_shading", strip, {{I::Material, "Material\nThe selected material asset (or the object's material)"}}, &only, {});
        } else if (ctx.vertical_tabs("prop_tabs", strip, items, &active, {object_start})) {
            prop_tab_ = tabs[static_cast<size_t>(active)].tab;
        }

        // Header: breadcrumb, as Blender shows "Scene > Cube > Material".
        const imm::Box content{area.x + strip_w, area.y, area.w - strip_w, area.h};
        const imm::Box hb{content.x, content.y, content.w, 26};
        std::string crumb = doc_.scene_name();
        I crumb_icon = I::Scene;
        if (prop_tab_ >= PropTab::Object && obj) { crumb += "  >  " + get_string(*obj, "name"); crumb_icon = object_icon_(*obj, ctx.style).first; }
        if (tab_ == Tab::Shading && material_.open()) { crumb = material_.ref; crumb_icon = I::Material; }
        ctx.icon(crumb_icon, {hb.x + 8, hb.y + 5, hb.h - 10, hb.h - 10}, ctx.style.text_dim);
        ctx.text_in({hb.x + hb.h + 6, hb.y, hb.w - hb.h - 6, hb.h}, crumb, ctx.style.text_dim, 0.0f);
        ctx.begin_region("prop_body", {content.x, hb.bottom(), content.w, content.h - hb.h}, true);
        if (tab_ == Tab::Shading) draw_shading_properties_(ctx);
        else switch (prop_tab_) {
            case PropTab::Tool:       draw_tool_tab_(ctx); break;
            case PropTab::Render:     draw_render_props_(ctx); break;
            case PropTab::Output:     draw_output_props_(ctx); break;
            case PropTab::Scene:      draw_scene_props_(ctx); break;
            case PropTab::World:      draw_world_props_(ctx); break;
            case PropTab::Object:     draw_object_props_(ctx, id); break;
            case PropTab::Components: draw_component_list_(ctx, id, false); break;
            case PropTab::Physics:    draw_component_list_(ctx, id, true); break;
            case PropTab::Data:       draw_data_props_(ctx, id); break;
            case PropTab::Material:   draw_material_props_(ctx, id); break;
        }
        ctx.end_region();
    }

    // --- global tabs -----------------------------------------------------------------

    void draw_tool_tab_(imm::Context& ctx) {
        using I = imm::Icon;
        if (in_sculpt_mode_()) { draw_sculpt_tool_panel_(ctx); return; }
        static const char* tool_names[] = {"Select Box", "Move", "Rotate", "Scale", "Cursor"};
        static const I tool_icons[] = {I::SelectBox, I::Move, I::Rotate, I::Scale, I::Cursor};
        const int t = static_cast<int>(tool_);
        if (ctx.collapsing_header("Active Tool", true, nullptr, tool_icons[t])) {
            ctx.label(tool_names[t]);
            int orient = static_cast<int>(orient_);
            if (ctx.combo("Orientation", &orient, {"Global", "Local", "Normal"})) orient_ = static_cast<Orientation>(orient);
            ctx.property_bool("Snap (Ctrl inverts)", &snap_on_);
            ctx.drag_float("Move Increment", &gizmo_.translate_snap, 0.01f, 0.001f, 100.0f);
            ctx.drag_float("Rotate Increment", &gizmo_.rotate_snap, 0.5f, 0.1f, 90.0f, "%.1f");
        }
        if (mesh_edit_view_() && mesh_.open()) draw_mesh_tools_(ctx);
        if (ctx.collapsing_header("Workspace", false, nullptr, I::Asset)) {
            ctx.property_bool("Grid", &show_grid_);
            ctx.property_bool("Gizmos", &show_gizmo_);
            ctx.property_bool("Toolbar (T)", &show_toolbar_);
            ctx.property_bool("Sidebar (N)", &show_sidebar_);
        }
    }

    /** @brief Draws settings groups (from settings_schema.h) against a config.yaml section. */
    void draw_setting_groups_(imm::Context& ctx, const std::vector<SettingsGroup>& groups, const std::string& section,
                              const std::vector<std::string>& only) {
        InspectorEnv env = inspector_env_();
        Node& sec = config_.section(section);
        for (const auto& g : groups) {
            if (!only.empty() && std::find(only.begin(), only.end(), g.title) == only.end()) continue;
            if (!ctx.collapsing_header(g.title, g.title == "Features" || g.title == "Viewport & Resolution" || g.title == "Lighting & Sky")) continue;
            ctx.indent(4);
            for (const auto& f0 : g.fields) {
                FieldDesc f = f0;
                if (f.startup_only) f.label = f.display() + " *";
                draw_setting_row_(ctx, f, sec, env, section);
            }
            ctx.unindent(4);
            ctx.spacing(4);
        }
    }

    void draw_render_props_(imm::Context& ctx) {
        using I = imm::Icon;
        if (ctx.button(config_.dirty() ? "Save config.yaml *" : "Save config.yaml", 150, true, I::Save)) save_config();
        ctx.same_line();
        if (ctx.button("Restart Renderer", 150, true, I::Restart)) restart_ = true;
        ctx.tooltip("Restart Renderer\nRebuilds the renderer so startup-only settings (marked *) apply. Open documents are kept.");
        ctx.label_dim("Changes apply live; * needs a restart. x resets a key to its preset.");
        ctx.spacing(4);
        draw_setting_groups_(ctx, render_settings_groups(), "render",
                             {"Viewport & Resolution", "Quality Tiers", "Features", "Shadows", "Bloom & Exposure", "Stylize", "Debug"});
        if (ctx.collapsing_header("Other Render Keys", false)) {
            InspectorEnv env = inspector_env_();
            const auto known = render_settings_keys();
            const std::set<std::string> skip(known.begin(), known.end());
            const Node before = config_.node;
            EditResult r = draw_fields(ctx, {}, config_.section("render"), env, true, skip);
            if (r.changed) { config_.commit("Edit render." + r.key, before, r.active ? "cfg:" + r.key : std::string()); apply_config_live(); }
            if (r.finished) config_.undo.end_merge();
        }
    }

    void draw_world_props_(imm::Context& ctx) {
        ctx.label_dim("The sky gradient lights every scene; fog is global.");
        ctx.spacing(4);
        draw_setting_groups_(ctx, render_settings_groups(), "render", {"Lighting & Sky", "Fog"});
    }

    void draw_output_props_(imm::Context& ctx) {
        using I = imm::Icon;
        if (ctx.button(config_.dirty() ? "Save config.yaml *" : "Save config.yaml", 150, true, I::Save)) save_config();
        ctx.same_line();
        if (ctx.button("Package Project...", 150, true, I::Package)) open_package_dialog_();
        ctx.tooltip("Package Project\nCopies assets/ with every YAML file encoded to .caml, ready to ship.");
        ctx.spacing(4);
        InspectorEnv env = inspector_env_();
        for (const auto& g : project_settings_groups()) {
            if (g.title == "Physics") continue;   // Scene tab
            if (!ctx.collapsing_header(g.title, g.title == "Window")) continue;
            Node& section = config_.section(project_section_key(g.title));
            ctx.indent(4);
            for (const auto& f : g.fields) draw_setting_row_(ctx, f, section, env, project_section_key(g.title));
            ctx.unindent(4);
        }
        ctx.label_dim("Window and jobs settings apply the next time the game starts.");
    }

    void draw_scene_props_(imm::Context& ctx) {
        using I = imm::Icon;
        if (ctx.collapsing_header("Scene", true, nullptr, I::Scene)) {
            std::string scene_name = doc_.scene_name();
            if (!playing() && ctx.input_text("Name", &scene_name)) apply_(doc_.set_scene_key("scene_name", Node(scene_name)));
            bool auto_t = get_bool(doc_.node().at("scene"), "auto_transform", true);
            if (!playing() && ctx.property_bool("Auto Transform", &auto_t)) apply_(doc_.set_scene_key("auto_transform", Node(auto_t)));
            if (doc_.node().at("scene").contains("inherit_from")) ctx.label_dim("Inherits " + get_string(doc_.node().at("scene"), "inherit_from"));
            ctx.label_dim(doc_.path().empty() ? std::string("Not saved yet") : project_.relative(doc_.path()));
        }
        if (ctx.collapsing_header("Startup", true, nullptr, I::Play)) {
            Node& sc = config_.section("scene");
            std::vector<std::string> scenes;
            for (const auto& s : project_.scenes()) scenes.push_back("assets/" + s);
            const std::string cur = get_string(sc, "default_scene");
            int idx = -1;
            for (size_t i = 0; i < scenes.size(); ++i) if (scenes[i] == cur) idx = static_cast<int>(i);
            if (idx < 0 && !cur.empty()) { scenes.push_back(cur); idx = static_cast<int>(scenes.size()) - 1; }
            const Node before = config_.node;
            if (ctx.combo("Default Scene", &idx, scenes) && idx >= 0) {
                sc["default_scene"] = Node(scenes[static_cast<size_t>(idx)]);
                config_.commit("Default Scene", before, {});
            }
            if (!doc_.path().empty() && ctx.button("Use This Scene at Startup", -1, true, I::Check)) {
                sc["default_scene"] = Node("assets/" + project_.relative(doc_.path()));
                config_.commit("Default Scene", before, {});
            }
        }
        if (ctx.collapsing_header("Physics", true, nullptr, I::Physics)) {
            InspectorEnv env = inspector_env_();
            Node& section = config_.section("physics");
            for (const auto& g : project_settings_groups()) {
                if (g.title != "Physics") continue;
                for (const auto& f : g.fields) draw_setting_row_(ctx, f, section, env, "physics");
            }
        }
    }

    // --- object tabs -----------------------------------------------------------------

    void draw_object_props_(imm::Context& ctx, ObjectId id) {
        using I = imm::Icon;
        const Node* obj = doc_.find(id);
        if (!obj) return;
        const bool editable = !playing();
        // Name row with the object's icon.
        {
            imm::Box row = ctx.next_box(ctx.style.row_height + 2);
            auto [icon, tint] = object_icon_(*obj, ctx.style);
            ctx.icon(icon, {row.x + 2, row.y + 3, row.h - 6, row.h - 6}, tint);
            std::string name = get_string(*obj, "name", "Object");
            if (editable && ctx.input_text_box("objname", {row.x + row.h + 2, row.y, row.w - row.h - 2, row.h}, &name) && !name.empty()) {
                apply_(doc_.set_object_key(id, "name", Node(name), "Rename"));
            }
        }
        if (ctx.collapsing_header("Transform", true, nullptr, I::Orientation)) {
            glm::vec3 p, r, s;
            doc_.get_transform(id, p, r, s);
            glm::vec3 np = p, nr = r, ns = s;
            bool active = false, finished = false;
            auto track = [&]() { active |= ctx.last_group_active(); finished |= ctx.last_deactivated(); };
            const bool cp = ctx.drag_float_stacked("Location", &np.x, 3, 0.02f, "%.3f m");
            track();
            const bool cr = ctx.drag_float_stacked("Rotation", &nr.x, 3, 0.5f, "%.1f");
            track();
            const bool cs = ctx.drag_float_stacked("Scale", &ns.x, 3, 0.01f, "%.3f");
            track();
            if ((cp || cr || cs) && editable) {
                apply_(doc_.set_transform(id, np, nr, ns, cp ? "Move" : cr ? "Rotate" : "Scale", active ? "props_transform" : std::string()));
            }
            if (finished) doc_.end_merge();
        }
        if (ctx.collapsing_header("Relations", true, nullptr, I::Link)) {
            std::vector<std::string> names = {"(none)"};
            std::vector<ObjectId> ids = {0};
            for (ObjectId other : doc_.all_ids()) {
                if (other == id || doc_.is_ancestor(id, other)) continue;
                names.push_back(get_string(*doc_.find(other), "name"));
                ids.push_back(other);
            }
            const ObjectId parent = doc_.parent_of(id).value_or(0);
            int idx = 0;
            for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == parent) idx = static_cast<int>(i);
            if (ctx.combo("Parent", &idx, names) && editable) {
                const glm::mat4 w = world_of_(id);
                const ObjectId np = ids[static_cast<size_t>(idx)];
                if (doc_.reparent(id, np).scope != ChangeScope::None) set_world_transform_(id, w, np ? world_of_(np) : glm::mat4(1.0f), "Keep Transform");
                queue_rebuild_();
            }
        }
        if (ctx.collapsing_header("Visibility", true, nullptr, I::Eye)) {
            bool shown = !hidden_.count(id);
            if (ctx.property_bool("Show in Viewport", &shown)) { if (shown) { hidden_.erase(id); if (auto* l = sync_.live(id)) l->set_active(get_bool(*doc_.find(id), "active", true)); } else hide_({id}); }
            ctx.tooltip("Show in Viewport\nEditor-only visibility (H / Alt H); not saved");
            bool enabled = get_bool(*doc_.find(id), "active", true);
            if (ctx.property_bool("Enabled in Game", &enabled) && editable) apply_(doc_.set_object_key(id, "active", Node(enabled), "Toggle Active"));
            ctx.tooltip("Enabled in Game\nThe object's `active` flag (Unity's GameObject.SetActive)");
        }
        if (obj->contains("inherit_from")) ctx.label_dim("Prefab: " + get_string(*obj, "inherit_from") + " (edits override)");
    }

    /**
     * @brief Unity's component list: every component (Transform aside) as a panel with its
     *        icon, a remove button and a context menu; "Add Component" with search.
     * @param physics True: only physics components (the Physics tab); false: all others.
     */
    void draw_component_list_(imm::Context& ctx, ObjectId id, bool physics) {
        using I = imm::Icon;
        const bool editable = !playing();
        InspectorEnv env = inspector_env_();
        const Node* obj = doc_.find(id);
        if (!obj || !obj->contains("components")) return;
        const size_t count = obj->at("components").size();
        bool any = false;
        for (size_t i = 0; i < count; ++i) {
            obj = doc_.find(id);
            if (!obj || i >= obj->at("components").size()) break;
            Node comp = obj->at("components").as_seq()[i];
            const std::string type = component_type(comp);
            if (type == "Transform") continue;
            const ComponentSchema* schema = find_schema(type);
            const bool is_physics = schema && schema->category == "Physics";
            if (is_physics != physics) continue;
            any = true;
            ctx.push_id(static_cast<int64_t>(i));
            bool remove = false;
            const bool removable = editable && (!schema || schema->removable);
            const bool open = ctx.collapsing_header(type.empty() ? std::string("(untyped)") : type, true, removable ? &remove : nullptr,
                                                    icon_for_component_(type));
            ctx.tooltip(type + "\nRight-click for more");
            if (editable) ctx.open_context_popup_on_last("comp_ctx");
            if (ctx.begin_popup("comp_ctx", 220)) {
                if (ctx.menu_item("Move Up", "", nullptr, i > 0, I::ArrowRight)) apply_(doc_.move_component(id, static_cast<int>(i), -1));
                if (ctx.menu_item("Move Down", "", nullptr, i + 1 < count, I::ArrowDown)) apply_(doc_.move_component(id, static_cast<int>(i), +1));
                if (ctx.menu_item("Reset", "", nullptr, schema != nullptr, I::Restart)) {
                    apply_(doc_.set_component(id, static_cast<int>(i), default_component(type), "Reset " + type));
                }
                if (ctx.menu_item("Copy as YAML", "", nullptr, true, I::Duplicate)) {
                    Node c = comp;
                    strip_private_keys(c);
                    if (ctx.input().set_clipboard) ctx.input().set_clipboard(coopa::yaml::emit(c));
                }
                if (ctx.menu_item("Remove Component", "", nullptr, removable, I::Trash)) remove = true;
                ctx.end_popup();
            }
            if (remove) { apply_(doc_.remove_component(id, static_cast<int>(i))); ctx.pop_id(); break; }
            if (open) {
                ctx.indent(6);
                const Node before = comp;
                EditResult r;
                if (type == "MeshRenderer") {
                    // The material lives on its own tab, as in Blender.
                    std::vector<FieldDesc> fields;
                    for (const auto& f : schema->fields) if (f.kind != FieldKind::Material) fields.push_back(f);
                    r = draw_fields(ctx, fields, comp, env, true, {"type", "id", "material"});
                    if (ctx.button("Material...", -1, true, I::Material)) prop_tab_ = PropTab::Material;
                } else {
                    r = draw_component(ctx, comp, env, [this](const std::string& rel) { open_material(project_.absolute(rel)); set_tab(Tab::Shading); });
                }
                ctx.unindent(6);
                if (r.changed && editable && !(comp == before)) {
                    const std::string key = "c" + std::to_string(id) + ":" + std::to_string(i) + ":" + r.key;
                    apply_(doc_.set_component(id, static_cast<int>(i), comp, "Edit " + type + "." + r.key, r.active ? key : std::string()));
                }
                if (r.finished) doc_.end_merge();
            }
            ctx.pop_id();
            ctx.spacing(5);
        }
        if (!any) ctx.label_dim(physics ? "No physics components." : "No components besides Transform.");
        if (!editable) return;
        ctx.spacing(6);
        if (ctx.button(physics ? "Add Physics Component" : "Add Component", -1, true, I::Plus)) {
            add_component_filter_.clear();
            ctx.open_popup(physics ? "add_phys_comp" : "add_comp", glm::vec2(ctx.last_rect().x, ctx.last_rect().bottom() + 2));
        }
        for (int which = 0; which < 2; ++which) {
            if (!ctx.begin_popup(which == 0 ? "add_comp" : "add_phys_comp", std::max(220.0f, ctx.last_rect().w))) continue;
            const bool phys_only = which == 1;
            imm::Box sb = ctx.next_box(ctx.style.row_height);
            ctx.input_text_box("comp_search", sb, &add_component_filter_, "Search...");
            std::map<std::string, std::vector<std::string>> by_cat;
            for (const auto& [t, sc] : schemas()) {
                if (t == "Transform" || (sc.unique && doc_.find_component(id, t) >= 0)) continue;
                if (phys_only && sc.category != "Physics") continue;
                if (!add_component_filter_.empty()) {
                    std::string a = t, b = add_component_filter_;
                    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                    if (a.find(b) == std::string::npos) continue;
                }
                by_cat[sc.category].push_back(t);
            }
            for (const auto& [cat, types] : by_cat) {
                ctx.label_dim(cat);
                for (const auto& t : types) {
                    if (ctx.menu_item(t, "", nullptr, true, icon_for_component_(t))) apply_(doc_.add_component(id, default_component(t)));
                }
            }
            if (by_cat.empty()) ctx.label_dim("Nothing matches.");
            ctx.end_popup();
        }
    }

    void draw_data_props_(imm::Context& ctx, ObjectId id) {
        using I = imm::Icon;
        const int ci = doc_.find_component(id, "MeshRenderer");
        if (ci < 0) return;
        Node comp = doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
        InspectorEnv env = inspector_env_();
        if (ctx.collapsing_header("Mesh", true, nullptr, I::Mesh)) {
            const ComponentSchema* sc = find_schema("MeshRenderer");
            std::vector<FieldDesc> fields;
            for (const auto& f : sc->fields) if (f.key == "mesh_path") fields.push_back(f);
            const Node before = comp;
            EditResult r = draw_fields(ctx, fields, comp, env, false);
            if (r.changed && !(comp == before) && !playing()) apply_(doc_.set_component(id, ci, comp, "Change Mesh"));
            if (const CachedMesh* cm = mesh_for_object_(*doc_.find(id))) {
                ctx.label_dim(std::to_string(cm->mesh.positions.size()) + " vertices   " + std::to_string(cm->edges.size()) + " edges   " +
                              std::to_string(cm->mesh.faces.size()) + " faces   " + std::to_string(cm->mesh.triangle_count()) + " triangles");
                const glm::vec3 d = cm->hi - cm->lo;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "Bounds  %.2f x %.2f x %.2f m", d.x, d.y, d.z);
                ctx.label_dim(buf);
            }
            if (!playing() && ctx.button(in_edit_mode_() ? "Leave Edit Mode (Tab)" : "Edit Mesh (Tab)", -1, true, I::EditMode)) toggle_edit_mode_();
        }
        if (ctx.collapsing_header("Level of Detail", false, nullptr, I::Zoom)) {
            const ComponentSchema* sc = find_schema("MeshRenderer");
            std::vector<FieldDesc> fields;
            for (const auto& f : sc->fields) if (f.key == "lod_bias" || f.key == "lods_enabled" || f.key == "affects_reflection_probes") fields.push_back(f);
            const Node before = comp;
            EditResult r = draw_fields(ctx, fields, comp, env, false);
            if (r.changed && !(comp == before) && !playing()) apply_(doc_.set_component(id, ci, comp, "Edit Mesh Renderer", r.active ? "lod" : std::string()));
            if (r.finished) doc_.end_merge();
        }
    }

    void draw_material_props_(imm::Context& ctx, ObjectId id) {
        using I = imm::Icon;
        const int ci = doc_.find_component(id, "MeshRenderer");
        if (ci < 0) return;
        Node comp = doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
        // Slot row with a shaded-ball preview, Blender-style.
        {
            imm::Box row = ctx.next_box(44);
            ctx.fill_rounded(row, ctx.style.panel_alt);
            const glm::vec3 alb = material_preview_color_(comp);
            ctx.circle({row.x + 22, row.y + 22}, 16, glm::vec4(alb, 1.0f));
            ctx.circle({row.x + 17, row.y + 17}, 6, glm::vec4(1, 1, 1, 0.35f));
            const std::string label = comp.contains("material") && comp.at("material").is_string() ? comp.at("material").get_value<std::string>()
                                    : comp.contains("material") && comp.at("material").contains("base") ? get_string(comp.at("material"), "base") + " (+ overrides)"
                                    : std::string("Inline material");
            ctx.text_in({row.x + 46, row.y, row.w - 46, row.h}, label, ctx.style.text, 0.0f);
        }
        InspectorEnv env = inspector_env_();
        const Node before = comp;
        EditResult r = draw_material_field(ctx, comp, env, [this](const std::string& rel) {
            open_material(project_.absolute(rel));
            set_tab(Tab::Shading);
        });
        if (r.changed && !(comp == before) && !playing()) {
            apply_(doc_.set_component(id, ci, comp, "Edit Material." + r.key, r.active ? "mat:" + r.key : std::string()));
        }
        if (r.finished) doc_.end_merge();
        ctx.spacing(6);
        if (comp.contains("material") && comp.at("material").is_mapping() && !comp.at("material").contains("base") && !playing()) {
            if (ctx.button("Save as Material Asset", -1, true, I::Save)) extract_material_(id, ci, comp);
            ctx.tooltip("Save as Material Asset\nWrites materials/<name>.yaml and references it from this object");
        }
    }

    /** @brief The colour a material slot's preview ball shows (inline albedo, or the asset's). */
    glm::vec3 material_preview_color_(const Node& comp) {
        if (!comp.contains("material")) return glm::vec3(0.7f);
        const Node& m = comp.at("material");
        std::string ref;
        if (m.is_string()) ref = m.get_value<std::string>();
        else if (m.is_mapping() && m.contains("albedo")) return get_color(m, "albedo", glm::vec3(0.8f));
        else if (m.is_mapping() && m.contains("base")) ref = get_string(m, "base");
        if (ref.empty()) return glm::vec3(0.8f);
        try {
            const fs::path p = coopa::yaml::resolve_variant(project_.absolute(ref + ".yaml"));
            if (coopa::yaml::document_exists(p)) return get_color(coopa::yaml::load_document(p), "albedo", glm::vec3(0.8f));
        } catch (...) {}
        return glm::vec3(0.8f);
    }

    /** @brief The Shading workspace's Properties: the open material asset. */
    void draw_shading_properties_(imm::Context& ctx) {
        using I = imm::Icon;
        if (!material_.open()) {
            ctx.label_dim("Pick a material in the list (or + to create one).");
            if (doc_.primary() && doc_.find_component(doc_.primary(), "MeshRenderer") >= 0) {
                ctx.spacing(4);
                ctx.label_dim("The selected object's material:");
                draw_material_props_(ctx, doc_.primary());
            }
            return;
        }
        if (ctx.collapsing_header("Surface", true, nullptr, I::Material)) draw_material_editor_(ctx);
    }
