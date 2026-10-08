// editor/app/ui/outliner.inl -- included inside EditorApp's class body.
//
// The Hierarchy: exactly what the open file holds. For a scene, the root row is the
// file's `scene:` block (named by scene_name) and its rows are root_objects in file order,
// nested children, then every component in order (Transform included). For an object asset
// the root row is the object itself. Prefab instances (`prefab:`) show a link icon; the
// children they inherit from the object asset are rows like any other (ui/instances.inl),
// dimmed until they override something, and an accent bar marks a row with overrides. Rows
// have eye (viewport visibility) and monitor (enabled in the game) toggles.

    void draw_outliner_(imm::Context& ctx, const imm::Box& area) {
        using I = imm::Icon;
        outliner_hovered_ = ctx.is_hovered(area);
        eye_rects_.clear();
        hierarchy_hovered_ = outliner_hovered_;
        const imm::Box hb = area_header_(ctx, area);
        ctx.icon(doc_.is_object_asset() ? I::Object : I::Scene, {hb.x + 6, hb.y + 4, hb.h - 8, hb.h - 8}, ctx.style.text_dim);
        // The area's title, then the filter in what's left of the header.
        const char* title = "Hierarchy";
        const float title_w = ctx.text_width(title) + 10;
        ctx.text_in({hb.x + hb.h + 2, hb.y, title_w, hb.h}, title, ctx.style.text, 0.0f);
        const float sx = hb.x + hb.h + 2 + title_w + 4;
        const imm::Box search{sx, hb.y + 4, std::max(40.0f, hb.right() - hb.h - 6 - sx), hb.h - 8};
        ctx.input_text_box("outliner_filter", search, &outliner_filter_, "    Filter");
        if (outliner_filter_.empty()) ctx.icon(I::Search, {search.x + 4, search.y + 3, search.h - 6, search.h - 6}, ctx.style.text_disabled);
        if (ctx.icon_button("outliner_new", I::Plus, "Add Object\nShift A in the viewport (object assets too)", false,
                            hb.h - 6, imm::Context::kAll, imm::Box{hb.right() - hb.h + 2, hb.y + 3, hb.h - 6, hb.h - 6})) {
            open_add_menu_ = true;   // declared by the viewport (draw_viewport_popups_)
        }
        const imm::Box body{area.x, hb.bottom(), area.w, area.h - hb.h};
        ctx.begin_region("outliner", body, true);
        // An object asset's file IS its object: no scene row above it.
        const bool object_file = doc_.is_object_asset();
        imm::TreeNodeResult root{};
        if (!object_file) {
            root = ctx.tree_node(ctx.get_id("scene_root"), doc_.scene_name(), false, false, true, nullptr, I::Scene);
            ctx.tooltip(doc_.scene_name() + "\nThe scene file's `scene:` block -- click for its settings (Properties > Scene)");
            if (root.clicked) { doc_.clear_selection(); prop_tab_ = PropTab::Scene; }
            if (root.right_clicked && !playing()) { context_target_ = 0; ctx.open_popup("outliner_ctx"); }
        } else {
            root.open = true;
        }
        if (root.open) {
            if (outliner_filter_.empty()) draw_outliner_list_(ctx, doc_.root_objects());
            else {
                std::string f = outliner_filter_;
                std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                doc_.for_each_object([&](const Node& o, int) {
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
        if (!object_file && outliner_filter_.empty() && active_type_ == AssetType::Scene) draw_outliner_runtime_(ctx);
        // Drop on empty space: un-parent to the root.
        if (!playing()) {
            const float y = ctx.cursor().y;
            if (auto dropped = ctx.drop_target("object", imm::Box{body.x, y, body.w, std::max(16.0f, body.bottom() - y)})) {
                apply_(doc_.reparent(std::stoll(*dropped), 0));
            }
            if (ctx.open_context_popup_in("outliner_ctx", body)) context_target_ = 0;
        }
        if (ctx.begin_popup("outliner_ctx", 240)) {
            draw_object_context_menu_(ctx, context_target_);
            ctx.end_popup();
        }
        ctx.end_region();
    }

    void draw_outliner_list_(imm::Context& ctx, const Node& list) {
        if (!list.is_sequence()) return;
        std::vector<ObjectId> ids;
        for (const auto& o : list.as_seq()) ids.push_back(SceneDocument::id_of(o));
        for (ObjectId id : ids) {
            if (outliner_row_skippable_(ctx, id, false)) ctx.next_box(ctx.style.row_height);
            else draw_outliner_row_(ctx, id, false);
        }
    }

    /**
     * @brief True for a row that is off screen and collapsed: the caller reserves its height
     *        instead of drawing it. A 1000-object scene then pays for the rows in view, not for
     *        every row's widgets, icons and strings each frame -- while scrolling, the content
     *        height and drop targets stay exactly as if every row were drawn. An open row is
     *        always drawn (its children's height isn't known without walking them), and so is
     *        the row being renamed.
     */
    bool outliner_row_skippable_(imm::Context& ctx, ObjectId id, bool flat) {
        if (rename_id_ == id || !ctx.next_row_clipped(ctx.style.row_height)) return false;
        if (flat) return true;   // filter results are flat rows: never open
        ctx.push_id(static_cast<int64_t>(id));
        const bool open = ctx.tree_is_open(ctx.get_id("row"));
        ctx.pop_id();
        return !open;
    }

    void draw_outliner_row_(imm::Context& ctx, ObjectId id, bool flat) {
        using I = imm::Icon;
        const Node* o = doc_.find(id);
        if (!o) return;
        const std::string name = get_string(*o, "name", "Object");
        const bool instance = SceneDocument::is_instance_node(*o);
        const bool inherited = o->contains(kInheritedKey);
        const Node& shown = effective_(*o);   // inside an instance: the asset merged with the overrides
        const bool overridden = (instance || inherited) && has_overrides_(id);
        const bool active = get_bool(shown, "active", true);
        const bool isolated = isolated_.count(id) > 0;   // hidden by Edit / Sculpt Mode isolation
        const bool hidden = hidden_.count(id) > 0 || isolated;
        std::vector<std::string> comps;
        if (shown.contains("components")) {
            for (const auto& c : shown.at("components").as_seq()) comps.push_back(component_type(c));
        }
        const bool has_children = !flat && o->contains("children") && o->at("children").is_sequence() && o->at("children").size() > 0;
        const bool leaf = flat || (!has_children && comps.empty());
        ctx.push_id(static_cast<int64_t>(id));

        if (rename_id_ == id) {
            imm::Box row = ctx.next_box(ctx.style.row_height);
            std::string n = name;
            if (rename_frames_++ == 0) ctx.begin_text_edit("rename", n);
            if (ctx.input_text_box("rename", {row.x + 20, row.y, row.w - 20, row.h}, &n) && !n.empty()) {
                apply_(doc_.set_object_key(id, "name", Node(n), "Rename"));
            }
            if (!ctx.wants_keyboard() && rename_frames_ > 1) rename_id_ = 0;
            ctx.pop_id();
            return;
        }

        auto [icon, tint] = object_icon_(shown, ctx.style);
        if (instance) { icon = I::Link; tint = ctx.style.accent; }
        glm::vec4 text_col = ctx.style.text;
        if (inherited && !overridden) text_col = ctx.style.text_dim;   // straight from the object asset
        if (!active || hidden) { text_col = ctx.style.text_disabled; tint = imm::with_alpha(tint, 0.45f); }
        const bool selected = doc_.is_selected(id);
        const bool is_active = doc_.primary() == id;
        // Row label tinted like Blender: the active object's name brighter.
        const glm::vec4 label_col = is_active ? et_.outliner.active_label : text_col;
        auto r = ctx.tree_node(ctx.get_id("row"), name, leaf, selected, false, &label_col, icon, 46.0f, is_active, &tint);
        if (r.clicked) {
            const bool add = has(ctx.input().mods, Mods::Shift) || has(ctx.input().mods, imm::Context::command_mod());
            doc_.select(id, add);
        }
        if (overridden) ctx.fill({r.rect.x, r.rect.y + 2, 2, r.rect.h - 4}, ctx.style.accent);   // has overrides
        if (r.double_clicked && !playing() && !inherited) { rename_id_ = id; rename_frames_ = 0; }
        if (r.right_clicked) { context_target_ = id; if (!doc_.is_selected(id)) doc_.select(id); ctx.open_popup("outliner_row_ctx"); }
        if (!playing()) {
            if (!inherited) ctx.drag_source("object", std::to_string(id), name);   // the asset places its parts
            if (auto dropped = ctx.drop_target("object", r.rect)) {
                const ObjectId src = std::stoll(*dropped);
                if (src != id) apply_(doc_.reparent(src, id));
            }
            if (active_type_ == AssetType::UI) {
                // The UI designer's palette and UI assets drop onto a row: added as its child.
                if (auto w = ctx.drop_target("ui_widget", r.rect)) ui_add_widget_(*w, id, std::nullopt);
                if (auto a = ctx.drop_target("asset", r.rect); a && asset_type_of_(*a) == AssetType::UI) place_ui_asset(*a, id);
            }
        }
        // Row toggles: eye (hide in viewport) and monitor (enabled in the game).
        const float s = ctx.style.row_height - 2;
        const imm::Box eye{r.rect.right() - s * 2 - 6, r.rect.y + 1, s, s};
        const imm::Box mon{r.rect.right() - s - 4, r.rect.y + 1, s, s};
        eye_rects_[id] = eye;
        if (ctx.icon_button("eye", hidden ? I::EyeClosed : I::Eye,
                            isolated ? "Hidden by Isolation\nEdit / Sculpt Mode is showing only the edited object (header toggle)"
                                     : "Hide in Viewport\nH hides, Alt H reveals (editor only, not saved)",
                            false, s, imm::Context::kAll, eye) && !isolated) {
            if (hidden) { hidden_.erase(id); if (auto* live = sync_.live(id)) live->set_active(active); }
            else hide_({id});
        }
        if (ctx.icon_button("mon", active ? I::Monitor : I::MonitorOff, "Enabled in Game\nThe object's `active` flag, saved with the scene",
                            false, s, imm::Context::kAll, mon) && !playing()) {
            apply_(doc_.set_object_key(id, "active", Node(!active), active ? "Disable Object" : "Enable Object"));
        }
        if (ctx.begin_popup("outliner_row_ctx", 240)) {
            draw_object_context_menu_(ctx, context_target_);
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
                auto cr = ctx.tree_node(ctx.get_id("comp"), comps[i], true, false, false, &dim, icon_for_component_(comps[i]));
                if (cr.clicked) {
                    doc_.select(id);
                    const ComponentSchema* sc = find_schema(comps[i]);
                    prop_tab_ = comps[i] == "Transform" ? PropTab::Object : comps[i] == "MeshRenderer" ? PropTab::Material
                              : (sc && sc->category == "Physics") ? PropTab::Physics : PropTab::Components;
                }
                ctx.tooltip(comps[i] + "\nComponent -- click to edit it in the Properties editor");
                ctx.pop_id();
            }
            ctx.tree_pop();
        }
        ctx.pop_id();
    }

    void draw_object_context_menu_(imm::Context& ctx, ObjectId target) {
        using I = imm::Icon;
        const bool ok = !playing();
        draw_instance_menu_items_(ctx, target);   // Open Object Asset, Revert / Apply overrides
        if (ctx.begin_menu("Add Child", ok && target != 0, I::Plus)) {
            if (ctx.menu_item("Empty", "", nullptr, true, I::Empty)) create_empty(target);
            for (const auto& p : primitive_names()) if (ctx.menu_item(p, "", nullptr, true, I::Cube)) create_primitive(p, target);
            if (ctx.menu_item("Point Light", "", nullptr, true, I::PointLight)) create_with_component("PointLight", "Point Light", target);
            ctx.end_menu();
        }
        if (ctx.begin_menu("Add", ok, I::Plus)) {
            if (ctx.menu_item("Empty", "", nullptr, true, I::Empty)) create_empty();
            for (const auto& p : primitive_names()) if (ctx.menu_item(p, "", nullptr, true, I::Cube)) create_primitive(p);
            ctx.end_menu();
        }
        if (ctx.begin_menu("Object Asset", ok && !doc_.is_object_asset(), I::Object)) {
            for (const auto& rel : list_assets_(AssetType::Object)) {
                if (ctx.menu_item(fs::path(rel).stem().string(), "", nullptr, true, I::Link)) place_object_asset(rel);
            }
            if (list_assets_(AssetType::Object).empty()) ctx.label_dim("No object assets yet");
            ctx.end_menu();
        }
        if (ctx.menu_item("Create Object Asset", "", nullptr, ok && target != 0 && !doc_.is_object_asset(), I::Object)) {
            doc_.select(target);
            create_object_asset_from_selection();
        }
        ctx.tooltip("Create Object Asset\nSave this object as objects/<name>.yaml and replace it with an instance");
        ctx.menu_separator();
        if (ctx.menu_item("Rename", "F2", nullptr, ok && target != 0 && !doc_.is_inherited(target))) { rename_id_ = target; rename_frames_ = 0; }
        if (ctx.menu_item("Duplicate", "Shift D", nullptr, ok && target != 0, I::Duplicate)) duplicate_selected();
        if (ctx.menu_item("Delete", "X", nullptr, ok && target != 0, I::Trash)) delete_selected();
        if (ctx.menu_item("Clear Parent", "Alt P", nullptr, ok && target != 0)) clear_parent_keep_transform_();
        ctx.menu_separator();
        if (ctx.menu_item("Hide", "H", nullptr, target != 0, I::EyeClosed)) hide_(doc_.selection());
        if (ctx.menu_item("Show All", "Alt H", nullptr, true, I::Eye)) unhide_all_();
        if (ctx.menu_item("Frame", "F / .", nullptr, target != 0, I::Zoom)) frame_selected();
    }
