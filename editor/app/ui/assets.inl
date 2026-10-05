// editor/app/ui/assets.inl -- included inside EditorApp's class body.
//
// The asset side of the editor -- what it is for: building a toyengine project's assets.
//   - The Asset panel (left): a tab per asset type (Scenes, Objects, Meshes, Materials,
//     Textures) listing the project's assets of that type. Clicking one OPENS it -- one asset
//     at a time (unsaved changes prompt first) -- and the viewer and Properties follow.
//   - Opening each type: scenes and object assets load into the scene document (object
//     assets as a one-object scene, see SceneDocument::is_object_asset()); meshes, materials
//     and textures show in the private preview ("lookdev") scene.
//   - Object assets (objects/*.yaml, the engine's `prefab:` references): create one from a
//     mesh or from a scene selection, and place instances in scenes.

    // =================================================================================
    // Asset types and listing
    // =================================================================================

    struct AssetTypeInfo {
        AssetType type;
        const char* label;      ///< tab label (plural)
        const char* singular;
        imm::Icon icon;
        const char* dir;        ///< folder under assets/
    };
    static const std::vector<AssetTypeInfo>& asset_types_() {
        using I = imm::Icon;
        static const std::vector<AssetTypeInfo> t = {
            {AssetType::Scene, "Scenes", "Scene", I::Scene, "scenes"},
            {AssetType::Object, "Objects", "Object", I::Object, "objects"},
            {AssetType::Mesh, "Meshes", "Mesh", I::Mesh, "meshes"},
            {AssetType::Material, "Materials", "Material", I::Material, "materials"},
            {AssetType::Texture, "Textures", "Texture", I::Image, "textures"},
            {AssetType::UI, "UI", "UI", I::UiCanvas, "ui"},
            {AssetType::Theme, "Themes", "Theme", I::Palette, "ui/themes"},
        };
        return t;
    }
    static const AssetTypeInfo& asset_type_info_(AssetType t) {
        for (const auto& i : asset_types_()) if (i.type == t) return i;
        return asset_types_()[0];
    }

    /** @brief The project's assets of a type (assets-relative paths, sorted). */
    std::vector<std::string> list_assets_(AssetType t) {
        switch (t) {
            case AssetType::Scene: return project_.scenes();
            case AssetType::Object: return project_.list("objects", ".yaml");
            case AssetType::Mesh: return project_.list("meshes", ".yaml");
            case AssetType::Material: return project_.list("materials", ".yaml");
            case AssetType::UI: {
                // ui/themes/ holds themes, not canvases.
                std::vector<std::string> out;
                for (const auto& p : project_.list("ui", ".yaml")) if (p.rfind("ui/themes/", 0) != 0) out.push_back(p);
                return out;
            }
            case AssetType::Theme: return project_.list("ui/themes", ".yaml");
            case AssetType::Texture: {
                std::vector<std::string> out;
                for (const auto& p : project_.list("textures", "")) {
                    const std::string e = fs::path(p).extension().string();
                    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga") out.push_back(p);
                }
                return out;
            }
            default: return {};
        }
    }

    /**
     * @brief toyengine's own assets of a type that the project doesn't have a copy of -- the
     *        read-only layer the Asset panel shows below the project's (empty when the project is
     *        toyengine itself, or while the panel's toyengine toggle is off).
     */
    std::vector<std::string> list_engine_assets_(AssetType t) {
        if (!show_engine_assets_ || project_.is_engine()) return {};
        switch (t) {
            case AssetType::Scene: return project_.engine_scenes();
            case AssetType::Object: return project_.list_engine("objects", ".yaml");
            case AssetType::Mesh: return project_.list_engine("meshes", ".yaml");
            case AssetType::Material: return project_.list_engine("materials", ".yaml");
            case AssetType::UI: {
                std::vector<std::string> out;
                for (const auto& p : project_.list_engine("ui", ".yaml")) if (p.rfind("ui/themes/", 0) != 0) out.push_back(p);
                return out;
            }
            case AssetType::Theme: return project_.list_engine("ui/themes", ".yaml");
            case AssetType::Texture: {
                std::vector<std::string> out;
                for (const auto& p : project_.list_engine("textures", "")) {
                    const std::string e = fs::path(p).extension().string();
                    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga") out.push_back(p);
                }
                return out;
            }
            default: return {};
        }
    }

    /** @brief Materials a picker offers: the project's, then toyengine's (while shown). */
    std::vector<std::string> material_choices_() {
        std::vector<std::string> out = list_assets_(AssetType::Material);
        for (const auto& m : list_engine_assets_(AssetType::Material)) out.push_back(m);
        return out;
    }

public:
    /** @brief Shows / hides toyengine's assets in the Asset panel and pickers (remembered). */
    void set_show_engine_assets(bool on) {
        show_engine_assets_ = on;
        Node prefs = Project::load_prefs();
        prefs["show_engine_assets"] = Node(on);
        Project::save_prefs(prefs);
    }
    bool show_engine_assets() const { return show_engine_assets_; }
    /** @brief The Asset panel's engine rows for a type, as listed (tests). */
    std::vector<std::string> engine_assets_listed(AssetType t) { return list_engine_assets_(t); }

    /**
     * @brief Copies a read-only toyengine asset into the project (same path, so the copy now
     *        resolves instead of toyengine's) and opens it -- the way to edit one. A scene copies
     *        its whole folder (scene-local meshes and textures come along).
     */
    bool copy_engine_asset_to_project(AssetType t, const std::string& rel) {
        const fs::path src = coopa::yaml::resolve_variant(Project::engine_assets() / rel);
        std::error_code ec;
        if (!fs::exists(src, ec)) { log_error("Not a toyengine asset: " + rel); return false; }
        const fs::path dst = project_.assets() / fs::relative(src, Project::engine_assets(), ec);
        if (t == AssetType::Scene && src.filename().string().rfind("scene", 0) == 0) {
            fs::create_directories(dst.parent_path().parent_path(), ec);
            fs::copy(src.parent_path(), dst.parent_path(), fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        } else {
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(src, dst, fs::copy_options::skip_existing, ec);
            // A mesh's LOD sidecar travels with it.
            const fs::path lod = src.parent_path() / (src.stem().string() + ".lod" + src.extension().string());
            if (!ec && fs::exists(lod)) fs::copy_file(lod, dst.parent_path() / lod.filename(), fs::copy_options::skip_existing, ec);
        }
        project_.refresh();
        if (ec) { log_error("Copy to Project failed: " + ec.message()); return false; }
        log_info("Copied " + rel + " into the project -- this copy is editable and now replaces toyengine's");
        open_asset(t, project_.relative(dst));
        return true;
    }

private:
    /** @brief Refuses to open / edit a toyengine asset outside toyengine itself (logging why). */
    bool refuse_engine_asset_(const fs::path& abs) {
        if (!project_.is_engine_path(abs)) return false;
        log_warn(project_.relative(abs) + " is a read-only toyengine asset: drag it into a scene to use it, or right-click > "
                 "Copy to Project to edit it");
        return true;
    }

    /** @brief The asset type of an assets-relative path (by folder / extension). */
    static AssetType asset_type_of_(const std::string& rel) {
        const fs::path p(rel);
        const std::string ext = p.extension().string();
        const std::string g = p.generic_string();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") return AssetType::Texture;
        if (g.rfind("objects/", 0) == 0) return AssetType::Object;
        if (g.rfind("ui/themes/", 0) == 0) return AssetType::Theme;
        if (g.rfind("ui/", 0) == 0) return AssetType::UI;
        if (g.rfind("materials/", 0) == 0) return AssetType::Material;
        if (g.find("meshes/") != std::string::npos) return AssetType::Mesh;
        if (g.rfind("scenes/", 0) == 0) return AssetType::Scene;
        return AssetType::None;
    }

    /** @brief A display name for an asset: scenes show their folder, others the file stem. */
    static std::string asset_display_name_(AssetType t, const std::string& rel) {
        const fs::path p(rel);
        if (t == AssetType::Scene && p.filename().string().rfind("scene", 0) == 0 && p.has_parent_path()) {
            return p.parent_path().filename().string();
        }
        return p.stem().string();
    }

    /** @brief Is this (assets-relative) asset the one that's open? */
    bool asset_is_open_(AssetType t, const std::string& rel) const {
        if (t == AssetType::Theme) return game_theme_.open() && project_.relative(game_theme_.path) == rel;   // open beside a UI
        if (active_type_ != t) return false;
        if (t == AssetType::Mesh) return !mesh_.path.empty() && project_.relative(mesh_.path) == rel;
        return !active_path_.empty() && project_.relative(active_path_) == rel;
    }
    /** @brief Unsaved changes on the open asset (for the list's dot). */
    bool open_asset_dirty_() const {
        switch (active_type_) {
            case AssetType::Scene: case AssetType::Object: case AssetType::UI: return doc_.dirty();
            case AssetType::Mesh: return mesh_.open() && mesh_.dirty();
            case AssetType::Material: return material_.open() && material_.dirty();
            default: return false;
        }
    }

    // =================================================================================
    // Opening (one asset at a time)
    // =================================================================================

public:
    /**
     * @brief Opens an asset (assets-relative or absolute path) -- the one way in. Asks to
     *        save unsaved changes first; the viewer and Properties switch to the asset's type.
     */
    void open_asset(AssetType t, const std::string& item) {
        fs::path abs = project_.absolute(item);
        if (!fs::exists(abs) && !coopa::yaml::document_exists(abs) && !doc_.path().empty()) abs = doc_.path().parent_path() / item;
        if (refuse_engine_asset_(abs)) return;
        // A theme opens beside the open UI, replacing only the open theme (and, without a UI
        // open, the document it previews on) -- so only those need saving first.
        const bool replaces = t != AssetType::Theme || game_theme_.dirty() || (active_type_ != AssetType::UI && doc_.dirty());
        if (replaces) guarded_([this, t, abs] { open_asset_now_(t, abs); });
        else deferred_.push_back([this, t, abs] { open_asset_now_(t, abs); });
    }

private:
    void open_asset_now_(AssetType t, const fs::path& abs) {
        switch (t) {
            case AssetType::Scene: open_scene(abs); break;
            case AssetType::Object: open_object_asset(abs); break;
            case AssetType::Mesh: open_mesh(abs); break;
            case AssetType::Material: open_material(abs); break;
            case AssetType::Texture: open_texture(abs); break;
            case AssetType::UI: open_ui_asset(abs); break;
            case AssetType::Theme: open_theme(abs); break;
            default: log_info(abs.string()); break;
        }
    }

    /** @brief Legacy string kinds (drops, menus) -> open_asset(). */
    void open_asset_(const std::string& kind, const std::string& item) {
        if (kind == "scene") open_asset(AssetType::Scene, item);
        else if (kind == "object") open_asset(AssetType::Object, item);
        else if (kind == "mesh") open_asset(AssetType::Mesh, item);
        else if (kind == "material") open_asset(AssetType::Material, item);
        else if (kind == "texture") open_asset(AssetType::Texture, item);
        else if (kind == "ui") open_asset(AssetType::UI, item);
        else if (kind == "theme") open_asset(AssetType::Theme, item);
        else log_info(item);
    }

public:
    /** @brief Opens an object asset (objects/*.yaml) as a one-object scene. */
    bool open_object_asset(const fs::path& path) {
        // UI assets are object assets too, edited in the UI designer.
        if (asset_type_of_(project_.relative(path)) == AssetType::UI) return open_ui_asset(path);
        stop();
        try {
            doc_.load(path);
        } catch (const std::exception& e) {
            log_error(std::string("Open object failed: ") + e.what());
            return false;
        }
        if (!doc_.is_object_asset()) { log_error(project_.relative(path) + " is not an object asset (no `object:`)"); return false; }
        set_view_(AssetType::Object);
        active_path_ = path;
        sync_.fallback_path = path;
        rebuild_scene_();
        if (ObjectId root = doc_.object_root()) doc_.select(root);
        deferred_.push_back([this] { frame_all(); });
        log_info("Opened object " + project_.relative(path));
        return true;
    }

    /** @brief Creates objects/<name>.yaml (an empty object) and opens it. */
    bool new_object_asset(const std::string& name) {
        const std::string n = unique_asset_name_("objects", name);
        const fs::path path = project_.assets() / "objects" / (n + ".yaml");
        doc_.reset_object(n);
        try {
            fs::create_directories(path.parent_path());
            doc_.save(path);
            project_.refresh();
        } catch (const std::exception& e) {
            log_error(std::string("Create object failed: ") + e.what());
            return false;
        }
        return open_object_asset(path);
    }

    /** @brief Shows a texture asset in the viewer (texture editing is a TODO). */
    bool open_texture(const fs::path& path) {
        if (!fs::exists(path)) { log_error("Texture not found: " + path.string()); return false; }
        set_view_(AssetType::Texture);
        active_path_ = path;
        uploaded_revision_ = 0;
        deferred_.push_back([this] {
            ensure_preview_();
            camera_.axis_view('t', false);
            frame_all();
        });
        log_info("Opened texture " + project_.relative(path));
        return true;
    }

private:
    // =================================================================================
    // Object assets (prefabs)
    // =================================================================================

    /** @brief A clean object node: Transform at identity, no private keys. */
    static Node object_asset_node_(Node obj) {
        strip_private_keys(obj);
        if (obj.contains("components") && obj.at("components").is_sequence()) {
            for (auto& c : obj["components"].as_seq()) {
                if (component_type(c) != "Transform") continue;
                c = Node::mapping();
                c["type"] = Node(std::string("Transform"));
            }
        }
        return obj;
    }

    bool write_object_asset_(const fs::path& path, const Node& obj) {
        Node doc = Node::mapping();
        doc["format"] = Node(std::string("toyengine-object"));
        doc["object"] = obj;
        try {
            fs::create_directories(path.parent_path());
            coopa::yaml::save_document(path, doc);
            project_.refresh();
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Write object asset failed: ") + e.what());
            return false;
        }
    }

    /** @brief objects/<mesh>.yaml: an object with a MeshRenderer using `mesh_rel`; opens it. */
    void create_object_asset_from_mesh_(const std::string& mesh_rel) {
        const std::string stem = fs::path(mesh_rel).stem().string();
        const std::string n = unique_asset_name_("objects", stem);
        Node obj = doc_.make_object(n);
        Node mr = default_component("MeshRenderer");
        mr["mesh_path"] = Node(mesh_ref(mesh_rel));
        obj["components"].as_seq().push_back(mr);
        const fs::path path = project_.assets() / "objects" / (n + ".yaml");
        if (write_object_asset_(path, object_asset_node_(obj))) open_asset(AssetType::Object, project_.relative(path));
    }

public:
    /**
     * @brief Scene view: turns the selected object into objects/<name>.yaml and replaces it
     *        with an instance (`prefab:`) at the same place.
     */
    bool create_object_asset_from_selection() {
        const ObjectId id = doc_.primary();
        const Node* src = id ? doc_.find(id) : nullptr;
        if (!src || doc_.is_object_asset() || playing()) return false;
        const std::string name = get_string(*src, "name", "Object");
        const std::string n = unique_asset_name_("objects", name);
        const fs::path path = project_.assets() / "objects" / (n + ".yaml");
        if (!write_object_asset_(path, object_asset_node_(*src))) return false;
        Node transform = Node::mapping();
        transform["type"] = Node(std::string("Transform"));
        if (src->contains("components")) {
            for (const auto& c : src->at("components").as_seq()) if (component_type(c) == "Transform") transform = c;
        }
        apply_(doc_.edit("Create Object Asset", [&](Node&) -> Change {
            Node* o = doc_.find(id);
            if (!o) return {};
            Node inst = Node::mapping();
            inst["name"] = Node(name);
            inst["prefab"] = Node("objects/" + n);
            Node comps = Node::sequence();
            comps.as_seq().push_back(transform);
            inst["components"] = comps;
            inst[kEidKey] = o->at(kEidKey);
            *o = inst;
            return {ChangeScope::Structure, id};
        }));
        log_info("Created object asset objects/" + n + ".yaml");
        return true;
    }

    /** @brief Places an instance of an object asset (objects/x.yaml) in the open scene / object. */
    ObjectId place_object_asset(const std::string& object_rel, std::optional<glm::vec3> at = std::nullopt) {
        if (playing() || asset_view_()) return 0;
        const std::string stem = fs::path(object_rel).stem().string();
        const std::string ref = strip_yaml_ext(fs::path(object_rel).generic_string());   // objects/props/crate
        if (doc_.is_object_asset() && strip_yaml_ext(project_.relative(active_path_)) == ref) { log_warn("An object asset can't contain itself"); return 0; }
        Node obj = Node::mapping();
        obj["name"] = Node(doc_.unique_name(stem));
        obj["prefab"] = Node(ref);
        Node comps = Node::sequence();
        Node t = Node::mapping();
        t["type"] = Node(std::string("Transform"));
        t["position"] = make_vec3(at.value_or(spawn_point_()));
        comps.as_seq().push_back(t);
        obj["components"] = comps;
        const ObjectId id = doc_.add_object(obj, 0, -1, "Place " + stem);   // add_object stamps a fresh id
        after_structure_change_(id);
        return id;
    }

private:
    /** @brief Resolved node of a prefab instance (base merged with overrides), or the node. */
    Node resolved_object_(const Node& obj) const {
        if (!obj.contains("prefab") && !obj.contains("inherit_from")) return obj;
        Node clean = obj;
        strip_private_keys(clean);
        try {
            const std::string anchor = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).string();
            return coopa::scene::SceneInheritance::resolve_object(clean, anchor, [](const std::string& p) {
                return coopa::yaml::load_document(coopa::yaml::resolve_variant(p));
            });
        } catch (const std::exception&) {
            return obj;
        }
    }

    // =================================================================================
    // The Asset panel (left)
    // =================================================================================

    void draw_asset_panel_(imm::Context& ctx, const imm::Box& area) {
        using I = imm::Icon;
        const imm::Box hb = area_header_(ctx, area, 30);
        // Type tabs: one icon per asset type (tooltip names it).
        const auto& types = asset_types_();
        const float s = hb.h - 8;
        float x = hb.x + 6;
        for (size_t i = 0; i < types.size(); ++i) {
            const auto& t = types[i];
            const bool on = asset_tab_ == t.type;
            std::string tip = std::string(t.label) + "\nassets/" + t.dir + "/";
            if (ctx.icon_button(std::string("atab_") + t.label, t.icon, tip, on, s, imm::Context::kAll, imm::Box{x, hb.y + 4, s, s}, true)) {
                asset_tab_ = t.type;
            }
            x += s + 3;
        }
        const AssetTypeInfo& info = asset_type_info_(asset_tab_);
        // New (+), and left of it the toyengine toggle (a game project only).
        const imm::Box nb{hb.right() - s - 6, hb.y + 4, s, s};
        if (!project_.is_engine()) {
            const imm::Box eb{nb.x - s - 4, nb.y, s, s};
            if (ctx.icon_button("asset_engine", I::Package,
                                std::string(show_engine_assets_ ? "Hide" : "Show") + " toyengine's assets\n"
                                "Listed below the project's, read-only: drag them into a scene to use them, or "
                                "right-click > Copy to Project to edit one",
                                show_engine_assets_, s, imm::Context::kAll, eb, true)) {
                set_show_engine_assets(!show_engine_assets_);
            }
            test_rects_["asset_engine_toggle"] = eb;
        }
        const bool can_new = asset_tab_ != AssetType::Texture;
        if (ctx.icon_button("asset_new", I::Plus,
                            can_new ? std::string("New ") + info.singular + "\nCreate one in assets/" + info.dir + "/"
                                    : std::string("Import textures by copying image files into assets/textures/"),
                            false, s, imm::Context::kAll, nb) && can_new) {
            // A type with choices (mesh primitives, UI templates) opens them; the rest create directly.
            if (asset_new_has_choices_(asset_tab_)) ctx.open_popup("asset_new", glm::vec2(nb.x, nb.bottom()));
            else new_asset_(asset_tab_);
        }
        if (ctx.begin_popup("asset_new", 220)) {
            ctx.label_dim(std::string("New ") + info.singular);
            draw_new_asset_items_(ctx, asset_tab_);
            ctx.end_popup();
        }
        // Title + search.
        const imm::Box title{area.x + 8, hb.bottom() + 4, area.w - 16, 20};
        const auto items = list_assets_(asset_tab_);
        const auto engine_items = list_engine_assets_(asset_tab_);
        ctx.text_in(title, std::string(info.label) + "  (" + std::to_string(items.size()) + ")" +
                               (engine_items.empty() ? std::string() : "  + " + std::to_string(engine_items.size()) + " toyengine"),
                    ctx.style.text, 0.0f);
        const imm::Box search{area.x + 6, title.bottom() + 2, area.w - 12, 22};
        ctx.input_text_box("asset_filter", search, &asset_filter_, "    Search");
        if (asset_filter_.empty()) ctx.icon(I::Search, {search.x + 4, search.y + 4, 14, 14}, ctx.style.text_disabled);
        const imm::Box body{area.x, search.bottom() + 4, area.w, area.bottom() - search.bottom() - 4};
        ctx.begin_region("asset_list", body, true);
        std::string f = asset_filter_;
        std::transform(f.begin(), f.end(), f.begin(), ::tolower);
        size_t shown = 0;
        bool row_hovered = false;
        std::string right_clicked;
        for (const auto& rel : items) {
            const std::string name = asset_display_name_(asset_tab_, rel);
            std::string ln = rel;
            std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            if (!f.empty() && ln.find(f) == std::string::npos) continue;
            ++shown;
            ctx.push_id(rel);
            const bool open = asset_is_open_(asset_tab_, rel);
            const bool dirty = asset_tab_ == AssetType::Theme ? game_theme_.dirty() : open_asset_dirty_();
            const std::string label = name + (open && dirty ? "  *" : "");
            if (ctx.selectable(label, open, 0, info.icon)) {
                if (!open) open_asset(asset_tab_, rel);
                else if (asset_tab_ == AssetType::Theme) prop_tab_ = PropTab::Theme;
            }
            ctx.tooltip(name + "\nassets/" + rel + (asset_tab_ == AssetType::Object ? "\nDrag into a scene to place an instance" :
                                                    asset_tab_ == AssetType::Material ? "\nDrag onto an object or a mesh slot" :
                                                    asset_tab_ == AssetType::Mesh ? "\nDrag into a scene to add it" :
                                                    asset_tab_ == AssetType::UI ? "\nDrag into a scene (or another UI) to place it" :
                                                    asset_tab_ == AssetType::Theme ? "\nOpens in the Theme tab, previewed on a UI" : ""));
            ctx.drag_source("asset", rel, name);
            row_hovered |= ctx.last_hovered();
            test_rects_["asset_row:" + rel] = ctx.last_rect();
            if (ctx.last_clicked(imm::Mouse::Right)) right_clicked = rel;
            ctx.pop_id();
        }
        // toyengine's assets, read-only: drag and drop (and the right-click uses), no editing.
        bool engine_header = false;
        for (const auto& rel : engine_items) {
            const std::string name = asset_display_name_(asset_tab_, rel);
            std::string ln = rel;
            std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            if (!f.empty() && ln.find(f) == std::string::npos) continue;
            if (!engine_header) {
                engine_header = true;
                ctx.spacing(4);
                const imm::Box sep = ctx.next_box(18);
                ctx.fill({sep.x, sep.y + 2, sep.w, 1}, ctx.style.separator);
                ctx.icon(I::Lock, {sep.x + 2, sep.y + 5, 12, 12}, ctx.style.text_disabled);
                ctx.text_in({sep.x + 18, sep.y + 2, sep.w - 18, sep.h}, "toyengine  (read-only)", ctx.style.text_disabled, 0.0f);
            }
            ++shown;
            ctx.push_id("engine:" + rel);
            if (ctx.selectable(name, false, 0, info.icon)) refuse_engine_asset_(project_.absolute(rel));
            ctx.tooltip(name + "\ntoyengine: assets/" + rel + "\nRead-only -- drag it into a scene to use it, or right-click > "
                        "Copy to Project to edit a copy");
            ctx.drag_source("asset", rel, name);
            row_hovered |= ctx.last_hovered();
            test_rects_["asset_row:engine:" + rel] = ctx.last_rect();
            if (ctx.last_clicked(imm::Mouse::Right)) right_clicked = rel;
            ctx.pop_id();
        }
        // Opened OUTSIDE the row's push_id(): a popup's id is scoped like any widget's, so one
        // opened inside the row would never match the begin_popup("asset_ctx") below.
        if (!right_clicked.empty()) { asset_context_ = right_clicked; ctx.open_popup("asset_ctx"); }
        // Right-click on empty space in the list: Add (what + offers for this tab).
        else if (!row_hovered && ctx.is_hovered(body) && !ctx.popup_hovered() && ctx.input().released[1]) {
            ctx.open_popup("asset_list_ctx");
        }
        if (shown == 0) {
            ctx.label_dim(items.empty() && engine_items.empty() ? std::string("No ") + info.label + " yet -- press + to create one."
                                                                : "Nothing matches.");
            if (asset_tab_ == AssetType::Texture && items.empty()) ctx.label_dim("Copy .png files into assets/textures/.");
        }
        if (ctx.begin_popup("asset_ctx", 210)) {
            draw_asset_context_menu_(ctx, asset_tab_, asset_context_);
            ctx.end_popup();
        }
        if (ctx.begin_popup("asset_list_ctx", 200)) {
            const bool can_add = asset_tab_ != AssetType::Texture;
            if (asset_new_has_choices_(asset_tab_)) {
                if (ctx.begin_menu("Add", can_add, I::Plus)) {
                    test_rects_["asset_add"] = ctx.last_rect();
                    draw_new_asset_items_(ctx, asset_tab_);
                    ctx.end_menu();
                } else {
                    test_rects_["asset_add"] = ctx.last_rect();
                }
            } else {
                if (ctx.menu_item(std::string("Add ") + info.singular, "", nullptr, can_add, I::Plus)) new_asset_(asset_tab_);
                test_rects_["asset_add"] = ctx.last_rect();
            }
            ctx.tooltip(can_add ? std::string("Add\nCreate a new ") + info.singular + " in assets/" + info.dir + "/ (same as the + button)"
                                : std::string("Add\nImport textures by copying image files into assets/textures/"));
            ctx.menu_separator();
            if (ctx.menu_item("Refresh", "", nullptr, true, I::Restart)) project_.refresh();
            ctx.tooltip("Refresh\nRescan assets/ for files added or removed outside the editor");
            ctx.end_popup();
        }
        ctx.end_region();
        draw_asset_modals_(ctx);
    }

    void draw_asset_context_menu_(imm::Context& ctx, AssetType t, const std::string& rel) {
        using I = imm::Icon;
        const bool engine = project_.is_engine_asset(rel);
        ctx.label_dim(asset_display_name_(t, rel) + (engine ? "  (toyengine, read-only)" : ""));
        if (!engine && ctx.menu_item("Open", "", nullptr, true, asset_type_info_(t).icon)) open_asset(t, rel);
        if (t == AssetType::Object && ctx.menu_item("Place in Scene", "", nullptr, !asset_view_() && !playing(), I::Plus)) place_object_asset(rel);
        if (t == AssetType::UI && ctx.menu_item(active_type_ == AssetType::UI ? "Place in this UI" : "Place in Scene", "", nullptr,
                                                !asset_view_() && !playing() && !asset_is_open_(t, rel), I::Plus)) {
            place_ui_asset(rel, active_type_ == AssetType::UI ? doc_.object_root() : 0);
        }
        if (t == AssetType::Mesh && ctx.menu_item("Make Object Asset", "", nullptr, true, I::Object)) create_object_asset_from_mesh_(rel);
        if (t == AssetType::Mesh && ctx.menu_item("Add to Scene", "", nullptr, !asset_view_() && !playing(), I::Plus)) add_mesh_to_scene_(rel);
        if (t == AssetType::Material && ctx.menu_item("Assign to Selected", "", nullptr, !asset_view_() && !doc_.selection().empty(), I::Link)) {
            assign_material_(rel);
        }
        ctx.menu_separator();
        if (engine) {
            // Using it is fine; changing it is toyengine's business. A copy is the project's own.
            if (ctx.menu_item("Copy to Project", "", nullptr, true, I::Duplicate)) copy_engine_asset_to_project(t, rel);
            ctx.tooltip("Copy to Project\nCopies it into this project's assets/" + std::string(asset_type_info_(t).dir) +
                        "/ under the same name. The copy is editable and replaces toyengine's everywhere it is used.");
            ctx.menu_separator();
            if (ctx.menu_item("Copy Path", "", nullptr, true) && ctx.input().set_clipboard) ctx.input().set_clipboard(rel);
            return;
        }
        if (ctx.menu_item("Duplicate", "", nullptr, true, I::Duplicate)) duplicate_asset_(t, rel);
        if (ctx.menu_item("Rename...", "", nullptr, !asset_is_open_(t, rel))) { asset_rename_ = rel; asset_rename_to_ = fs::path(rel).stem().string(); pending_modal_ = "Rename Asset"; }
        if (ctx.menu_item("Delete...", "", nullptr, !asset_is_open_(t, rel), I::Trash)) { asset_delete_ = rel; pending_modal_ = "Delete Asset"; }
        test_rects_["asset_delete"] = ctx.last_rect();
        if (asset_is_open_(t, rel)) ctx.tooltip("Delete\nClose it first: open another asset, then delete this one");
        ctx.menu_separator();
        if (ctx.menu_item("Copy Path", "", nullptr, true) && ctx.input().set_clipboard) ctx.input().set_clipboard(rel);
    }

    /** @brief True when New for this type is a choice (mesh primitives, UI templates). */
    static bool asset_new_has_choices_(AssetType t) { return t == AssetType::Mesh || t == AssetType::UI; }

    /** @brief The New items of an asset type -- shared by the + button and the list's Add menu. */
    void draw_new_asset_items_(imm::Context& ctx, AssetType t) {
        using I = imm::Icon;
        if (t == AssetType::Mesh) {
            for (const auto& p : primitive_names()) {
                if (ctx.menu_item(p, "", nullptr, true, I::Mesh)) guarded_([this, p] { new_mesh(p); save_mesh(); project_.refresh(); });
                test_rects_["asset_new:" + p] = ctx.last_rect();
            }
            return;
        }
        if (t == AssetType::UI) {
            if (ctx.menu_item("Blank Canvas", "", nullptr, true, I::UiCanvas)) guarded_([this] { new_ui_asset("new_ui", "blank"); });
            ctx.tooltip("Blank Canvas\nA screen-space canvas (HUD, menu, screen) scaled from 1920 x 1080");
            test_rects_["asset_new:Blank Canvas"] = ctx.last_rect();
            if (ctx.menu_item("Blank Widget", "", nullptr, true, I::UiWidget)) guarded_([this] { new_ui_asset("new_widget", "widget"); });
            ctx.tooltip("Blank Widget\nA reusable piece (an item slot, a quest entry) placed inside other UI");
            const auto templates = ui_templates();
            if (!templates.empty()) ctx.menu_separator();
            for (const auto& tpl : templates) {
                std::string label = tpl;
                for (char& c : label) if (c == '_') c = ' ';
                if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
                if (ctx.menu_item(label, "", nullptr, true, I::UiWidget)) guarded_([this, tpl] { new_ui_asset(tpl, tpl); });
                ctx.tooltip(label + "\nStart from the " + label + " template (editor/templates/ui/" + tpl + ".yaml)");
            }
            return;
        }
        const AssetTypeInfo& info = asset_type_info_(t);
        if (ctx.menu_item(std::string("New ") + info.singular, "", nullptr, t != AssetType::Texture, info.icon)) new_asset_(t);
        test_rects_["asset_new:" + std::string(info.singular)] = ctx.last_rect();
    }

    void new_asset_(AssetType t) {
        switch (t) {
            case AssetType::Scene: guarded_([this] { new_scene_asset_("scene"); }); break;
            case AssetType::Object: guarded_([this] { new_object_asset("object"); }); break;
            case AssetType::Material: guarded_([this] { create_material("material"); }); break;
            case AssetType::UI: guarded_([this] { new_ui_asset("new_ui", "blank"); }); break;
            case AssetType::Theme: guarded_([this] { create_theme("theme"); }); break;
            default: break;
        }
    }

    /** @brief scenes/<name>/scene.yaml: the starter scene, saved, then opened. */
    void new_scene_asset_(const std::string& base) {
        std::string n = base;
        for (int i = 1; fs::exists(project_.assets() / "scenes" / n); ++i) n = base + "_" + std::to_string(i);
        const fs::path path = project_.assets() / "scenes" / n / "scene.yaml";
        new_scene();
        apply_(doc_.set_scene_key("scene_name", Node(n)));
        try {
            fs::create_directories(path.parent_path());
            save_scene_as(path);
        } catch (const std::exception& e) {
            log_error(std::string("Create scene failed: ") + e.what());
            return;
        }
        open_scene(path);
    }

    void duplicate_asset_(AssetType t, const std::string& rel) {
        const fs::path src = project_.absolute(rel);
        std::error_code ec;
        fs::path dst;
        if (t == AssetType::Scene && src.filename().string().rfind("scene", 0) == 0) {
            const fs::path dir = src.parent_path();
            fs::path ndir = dir;
            for (int i = 1; fs::exists(ndir); ++i) ndir = dir.parent_path() / (dir.filename().string() + "_" + std::to_string(i));
            fs::copy(dir, ndir, fs::copy_options::recursive, ec);
            dst = ndir / src.filename();
        } else {
            const std::string stem = src.stem().string();
            const fs::path dir = src.parent_path();
            dst = dir / (stem + "_copy" + src.extension().string());
            for (int i = 2; fs::exists(dst); ++i) dst = dir / (stem + "_copy" + std::to_string(i) + src.extension().string());
            fs::copy_file(src, dst, ec);
        }
        project_.refresh();
        if (ec) log_error("Duplicate failed: " + ec.message());
        else log_info("Duplicated to " + project_.relative(dst));
    }

    /** @brief Renames an asset and every reference to it; reloads open documents that changed. */
    void rename_asset_(const std::string& from, const std::string& to) {
        std::vector<std::string> rewritten;
        try {
            rewritten = project_.rename_asset(from, to);
        } catch (const std::exception& e) {
            log_error(std::string("Rename failed: ") + e.what());
            project_.refresh();
            return;
        }
        for (const auto& f : rewritten) log_info("Updated references in " + f);
        log_info("Renamed " + from + " to " + to);
        auto touched = [&](const fs::path& p) {
            return !p.empty() && std::find(rewritten.begin(), rewritten.end(), project_.relative(p)) != rewritten.end();
        };
        if ((active_type_ == AssetType::Scene || active_type_ == AssetType::Object || active_type_ == AssetType::UI) && touched(active_path_)) {
            open_asset_now_(active_type_, active_path_);
        }
        if (active_type_ == AssetType::Material && touched(material_.path)) open_material(material_.path);
        if (touched(config_.path)) config_.load(config_.path);
    }

    void draw_asset_modals_(imm::Context& ctx) {
        if (ctx.begin_modal("Rename Asset", {360, 0})) {
            ctx.label("Rename " + asset_rename_);
            ctx.input_text("New name", &asset_rename_to_);
            if (ctx.button("Rename", 100) && !asset_rename_to_.empty()) {
                const fs::path src = project_.absolute(asset_rename_);
                const fs::path dst = src.parent_path() / (asset_rename_to_ + src.extension().string());
                if (fs::exists(dst)) log_error(project_.relative(dst) + " already exists");
                // References are rewritten on disk, so unsaved edits are settled first.
                else guarded_([this, from = asset_rename_, to = project_.relative(dst)] { rename_asset_(from, to); });
                ctx.close_current_popup();
            }
            ctx.same_line();
            if (ctx.button("Cancel", 80)) ctx.close_current_popup();
            ctx.end_modal();
        }
        if (ctx.begin_modal("Delete Asset", {380, 0})) {
            ctx.label("Delete " + asset_delete_ + "?");
            ctx.label_dim("Scenes and objects referring to it will fail to load it.");
            const bool confirm = ctx.button("Delete", 100);
            test_rects_["asset_delete_confirm"] = ctx.last_rect();
            if (confirm) {
                std::error_code ec;
                const fs::path p = project_.absolute(asset_delete_);
                const bool scene_dir = asset_type_of_(asset_delete_) == AssetType::Scene && p.filename().string().rfind("scene", 0) == 0;
                if (scene_dir) fs::remove_all(p.parent_path(), ec);
                else fs::remove(p, ec);
                if (ec) log_error("Delete failed: " + ec.message());
                else log_info("Deleted " + asset_delete_);
                project_.refresh();
                ctx.close_current_popup();
            }
            ctx.same_line();
            if (ctx.button("Cancel", 80)) ctx.close_current_popup();
            ctx.end_modal();
        }
    }

    // =================================================================================
    // Console (under the viewer)
    // =================================================================================

    /** @brief The bottom area: Console and Timeline tabs. */
    void draw_console_area_(imm::Context& ctx, const imm::Box& area) {
        using I = imm::Icon;
        const imm::Box hb = area_header_(ctx, area, 24);
        float x = hb.x + 4;
        auto tab = [&](const char* id, I ic, const char* label, int index) {
            const float w = ctx.text_width(label) + hb.h + 12;
            const imm::Box b{x, hb.y + 2, w, hb.h - 4};
            bool hov = false, held = false;
            if (ctx.invisible_button(id, b, &hov, &held)) bottom_view_ = index;
            if (bottom_view_ == index) ctx.fill_rounded(b, ctx.style.panel_bg);
            else if (hov) ctx.fill_rounded(b, ctx.style.header_hover);
            const glm::vec4 c = bottom_view_ == index ? ctx.style.text : ctx.style.text_dim;
            ctx.icon(ic, {b.x + 4, b.y + 3, b.h - 6, b.h - 6}, c);
            ctx.text_in({b.x + b.h + 2, b.y, w - b.h, b.h}, label, c, 0.0f);
            x += w + 2;
        };
        tab("bt_console", I::Console, "Console", 0);
        tab("bt_timeline", I::Play, "Timeline", 1);
        const imm::Box body{area.x, hb.bottom(), area.w, area.h - hb.h};
        if (bottom_view_ == 1) {
            draw_timeline_(ctx, hb, body, x + 10);
        } else {
            timeline_hovered_ = false;
            draw_console_(ctx, hb, body);
        }
    }

    // =================================================================================
    // The preview ("lookdev") scene: meshes, materials, textures
    // =================================================================================

    /** @brief Lookdev settings (Material Properties > Preview). */
    struct Lookdev {
        int shape = 0;            ///< 0 shader ball, 1 sphere, 2 rounded cube, 3 plane, 4 cylinder
        bool turntable = false;
        bool ground = true;
        float angle = 0.0f;
        uint64_t revision = 1;    ///< bumped when the shape changes
    };

    static Node lookdev_object_(const std::string& name) {
        Node o = Node::mapping();
        o["name"] = Node(name);
        Node comps = Node::sequence();
        Node t = Node::mapping();
        t["type"] = Node(std::string("Transform"));
        comps.as_seq().push_back(t);
        o["components"] = comps;
        return o;
    }

    /**
     * @brief The preview scene: a studio rig (key sun with shadows, fill and rim point lights,
     *        an environment light), a ground disc and the preview object.
     */
    void ensure_preview_() {
        if (preview_scene_) return;
        Node doc = Node::mapping();
        Node scene = Node::mapping();
        scene["scene_name"] = Node(std::string("EditorPreview"));
        Node roots = Node::sequence();
        Node key = lookdev_object_("LookdevKey");
        Node l = Node::mapping();
        l["type"] = Node(std::string("DirectionalLight"));
        l["direction"] = make_vec3({-0.45f, -0.55f, -0.7f});
        l["intensity"] = make_float(1.3);
        l["cast_shadows"] = Node(true);
        key["components"].as_seq().push_back(l);
        roots.as_seq().push_back(key);
        auto point = [&](const std::string& name, glm::vec3 pos, double intensity) {
            Node o = lookdev_object_(name);
            o["components"].as_seq()[0]["position"] = make_vec3(pos);
            Node pl = Node::mapping();
            pl["type"] = Node(std::string("PointLight"));
            pl["intensity"] = make_float(intensity);
            pl["range"] = make_float(30.0);
            o["components"].as_seq().push_back(pl);
            roots.as_seq().push_back(o);
        };
        point("LookdevFill", {3.0f, 4.0f, 3.0f}, 30.0);
        point("LookdevRim", {-3.0f, -3.5f, 2.5f}, 25.0);
        Node env = lookdev_object_("LookdevEnvironment");
        Node el = Node::mapping();
        el["type"] = Node(std::string("EnvironmentLight"));
        env["components"].as_seq().push_back(el);
        roots.as_seq().push_back(env);
        auto renderer = [&](const std::string& name, glm::vec3 albedo, double rough) {
            Node o = lookdev_object_(name);
            Node mr = Node::mapping();
            mr["type"] = Node(std::string("MeshRenderer"));
            Node mat = Node::mapping();
            mat["albedo"] = make_color(albedo);
            mat["roughness"] = make_float(rough);
            mr["material"] = mat;
            o["components"].as_seq().push_back(mr);
            roots.as_seq().push_back(o);
        };
        renderer("PreviewGround", glm::vec3(0.28f), 0.9);
        renderer("PreviewObject", glm::vec3(0.72f), 0.55);
        scene["root_objects"] = roots;
        doc["scene"] = scene;
        try {
            coopa::scene::Scene s = coopa::scene::SceneLoader::load_from_node(doc, (project_.assets() / "__preview__.yaml").string());
            preview_scene_ = &engine_.push_scene(std::move(s), false);
            preview_object_ = preview_scene_->find_object("PreviewObject");
            preview_ground_ = preview_scene_->find_object("PreviewGround");
            uploaded_revision_ = 0;
            if (preview_ground_) {
                if (auto* gmr = preview_ground_->get_component<coopa::gfx::engine::components::MeshRenderer>()) {
                    const EditMesh disc = make_cylinder(1.8f, 0.04f, 48, true);
                    auto cpu = coopa::gfx::engine::data::Mesh::build_cpu(mesh_to_node(disc));
                    auto mesh = std::make_shared<coopa::gfx::engine::data::Mesh>(
                        coopa::gfx::engine::data::Mesh::from_cpu(engine_.device(), engine_.allocator(), std::move(cpu)));
                    gmr->set_mesh(engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/lookdev_ground", std::move(mesh)));
                }
            }
        } catch (const std::exception& e) {
            log_error(std::string("Preview scene failed: ") + e.what());
        }
    }

    /** @brief The lookdev shape for a material (object space, resting on z = 0 after placement). */
    EditMesh lookdev_shape_() const {
        switch (lookdev_.shape) {
            case 1: return make_uv_sphere(0.5f, 48, 24);
            case 2: {
                EditMesh m = make_cube();
                MeshSelection all;
                all.select_all(m);
                bevel_edges(m, all, 0.12f);
                for (auto& f : m.faces) f.smooth = true;
                return m;
            }
            case 3: return make_plane(1.2f, 1);
            case 4: return make_cylinder(0.45f, 1.0f, 48, true);
            default: return make_shader_ball();   // curves, flat planes and hard edges in one shape
        }
    }

    /** @brief Places the preview object so it rests on the ground disc (z = 0). */
    void place_preview_(const EditMesh& m, bool on_ground) {
        if (!preview_object_ || !preview_object_->get_transform()) return;
        glm::vec3 lo, hi;
        m.bounds(lo, hi);
        const float lift = on_ground ? -lo.z : 0.0f;
        preview_object_->get_transform()->transform().set_position(glm::vec3(0, 0, lift));
        if (preview_ground_ && preview_ground_->get_transform()) {
            preview_ground_->get_transform()->transform().set_position(glm::vec3(0, 0, -0.025f));
            preview_ground_->set_active(on_ground && lookdev_.ground);
        }
    }

    /** @brief Per frame while a preview asset is open: keep the preview object current. */
    void update_preview_() {
        auto* mr = preview_renderer_();
        if (!mr) return;
        switch (active_type_) {
            case AssetType::Mesh:
                if (!sculpt_active_ && !paint_active_ && uploaded_revision_ != mesh_.geometry_revision) {
                    upload_preview_mesh_(mesh_.mesh, "editor/preview_mesh");
                    refresh_material_preview_();
                    apply_slot_preview_materials_();
                    place_preview_(mesh_.mesh, false);
                    if (preview_ground_) preview_ground_->set_active(false);   // edit in empty space
                    uploaded_revision_ = mesh_.geometry_revision;
                }
                break;
            case AssetType::Material:
                if (uploaded_revision_ != lookdev_.revision) {
                    const EditMesh shape = lookdev_shape_();
                    upload_preview_mesh_(shape, "editor/lookdev_shape");
                    refresh_material_preview_();
                    place_preview_(shape, true);
                    uploaded_revision_ = lookdev_.revision;
                }
                if (lookdev_.turntable && preview_object_ && preview_object_->get_transform()) {
                    lookdev_.angle += 0.4f * (1.0f / 60.0f);
                    preview_object_->get_transform()->transform().set_rotation_quat(glm::angleAxis(lookdev_.angle, glm::vec3(0, 0, 1)));
                }
                break;
            case AssetType::Texture:
                if (uploaded_revision_ == 0) {
                    const EditMesh plane = make_plane(1.0f, 1);
                    upload_preview_mesh_(plane, "editor/texture_plane");
                    refresh_texture_preview_();
                    place_preview_(plane, false);
                    if (preview_ground_) preview_ground_->set_active(false);
                    uploaded_revision_ = 1;
                }
                break;
            default: break;
        }
    }

    /**
     * @brief How the project uses a texture: "linear" when a material, scene or object asset names
     *        it as a normal / metallic-roughness / alpha-mask map (data, not colour), "srgb" when
     *        as an albedo map; with no reference, guessed from the file name (_normal, _mr...).
     *
     * The preview must declare the SAME color space the project's materials will: the texture
     * loader keeps the first declaration for a file, so previewing a normal map as sRGB would
     * mis-decode it for every material that uses it for the rest of the session.
     */
    std::string texture_color_space_(const std::string& rel) {
        const std::string file = fs::path(rel).filename().string();
        int linear = 0, srgb = 0;
        std::function<void(const Node&)> scan = [&](const Node& n) {
            if (n.is_mapping()) {
                for (const auto& kv : n.as_map()) {
                    const std::string k = kv.first.is_string() ? kv.first.get_value<std::string>() : std::string();
                    if (kv.second.is_string() && k.rfind("texture_", 0) == 0) {
                        const std::string v = kv.second.get_value<std::string>();
                        if (v == rel || fs::path(v).filename().string() == file) (k == "texture_albedo" ? srgb : linear)++;
                    } else {
                        scan(kv.second);
                    }
                }
            } else if (n.is_sequence()) {
                for (const auto& e : n.as_seq()) scan(e);
            }
        };
        std::vector<std::string> docs = project_.list("materials", ".yaml");
        for (const auto& sc : project_.scenes()) docs.push_back(sc);
        for (const auto& o : project_.list("objects", ".yaml")) docs.push_back(o);
        for (const auto& d : docs) {
            try { scan(coopa::yaml::load_document(coopa::yaml::resolve_variant(project_.absolute(d)))); } catch (...) {}
        }
        if (linear || srgb) return linear > srgb ? "linear" : "srgb";
        std::string low = file;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        for (const char* tag : {"normal", "_nrm", "_n.", "_mr", "metal", "rough", "_ao", "occlusion", "mask", "height", "_orm"}) {
            if (low.find(tag) != std::string::npos) return "linear";
        }
        return "srgb";
    }

    /** @brief The texture view's material: the image as albedo (decoded in the color space the
     *         project uses it in -- see texture_color_space_()), fully rough. */
    void refresh_texture_preview_() {
        auto* mr = preview_renderer_();
        if (!mr) return;
        // TODO(texture-editor): a texture editor (channel view, resize, compression / sampler
        // settings, painting) belongs here and in draw_texture_properties_(); for now the
        // viewer shows the image on a plane.
        coopa::scene::SceneLoader::ParseContext ctx;
        ctx.scene_path = (project_.assets() / "__preview__.yaml").string();
        ctx.scene_dir = project_.assets().string();
        ctx.search_dirs = {ctx.scene_dir};
        Node m = Node::mapping();
        m["albedo"] = make_color(glm::vec3(1.0f));
        m["roughness"] = make_float(1.0);
        m["metallic"] = make_float(0.0);
        const std::string rel = project_.relative(active_path_);
        m["texture_albedo"] = Node(rel);
        Node spaces = Node::mapping();
        spaces[rel] = Node(texture_color_space_(rel));
        m["texture_color_space"] = spaces;
        mr->material = coopa::gfx::engine::components::PBRMaterial{};
        try {
            coopa::gfx::engine::components::parse_material_value_(m, mr->material, engine_.assets(), ctx);
        } catch (const std::exception& e) {
            log_error(std::string("Texture: ") + e.what());
        }
    }

    /** @brief A distinct preview colour per mesh slot (slot 0 stays the neutral grey). */
    static glm::vec3 slot_color_(uint32_t slot) {
        static const glm::vec3 palette[] = {{0.72f, 0.72f, 0.72f}, {0.85f, 0.45f, 0.25f}, {0.30f, 0.55f, 0.85f}, {0.45f, 0.75f, 0.35f},
                                            {0.80f, 0.70f, 0.25f}, {0.65f, 0.40f, 0.80f}, {0.30f, 0.75f, 0.75f}, {0.85f, 0.35f, 0.55f}};
        return palette[slot % 8];
    }

    /** @brief The mesh viewer's per-slot materials: a picked material asset, else a palette colour. */
    void apply_slot_preview_materials_() {
        auto* mr = preview_renderer_();
        if (!mr) return;
        coopa::scene::SceneLoader::ParseContext ctx;
        ctx.scene_path = (project_.assets() / "__preview__.yaml").string();
        ctx.scene_dir = project_.assets().string();
        ctx.search_dirs = {ctx.scene_dir};
        const uint32_t n = std::max<uint32_t>(1, static_cast<uint32_t>(mesh_.mesh.slots.size()));
        mr->slot_materials.clear();
        for (uint32_t s = 0; s < n; ++s) {
            coopa::gfx::engine::components::PBRMaterial pm;
            Node m = Node::mapping();
            m["albedo"] = make_color(slot_color_(s));
            m["roughness"] = make_float(0.55);
            try {
                if (s < slot_preview_materials_.size() && !slot_preview_materials_[s].empty()) {
                    coopa::gfx::engine::components::parse_material_value_(Node(slot_preview_materials_[s]), pm, engine_.assets(), ctx);
                } else {
                    coopa::gfx::engine::components::parse_material_value_(m, pm, engine_.assets(), ctx);
                }
            } catch (const std::exception& e) {
                log_warn(std::string("Slot preview material: ") + e.what());
            }
            mr->material_for_mut(s) = pm;
        }
    }

    /** @brief Studio lights for an object asset's view (live scene, never saved). */
    void add_object_view_lights_() {
        if (!doc_.is_object_asset() || !sync_.scene() || active_type_ == AssetType::UI) return;
        Node key = lookdev_object_("__ObjectViewKey");
        Node l = Node::mapping();
        l["type"] = Node(std::string("DirectionalLight"));
        l["direction"] = make_vec3({-0.45f, -0.55f, -0.7f});
        l["intensity"] = make_float(1.3);
        l["cast_shadows"] = Node(true);
        key["components"].as_seq().push_back(l);
        Node env = lookdev_object_("__ObjectViewEnvironment");
        Node el = Node::mapping();
        el["type"] = Node(std::string("EnvironmentLight"));
        env["components"].as_seq().push_back(el);
        for (const Node* n : {&key, &env}) {
            try {
                auto obj = coopa::scene::SceneLoader::build_object(*n, nullptr, (project_.assets() / "__object_view__.yaml").string());
                coopa::scene::SceneObject* raw = sync_.scene()->add_root_object(std::move(obj));
                sync_.scene()->adopt(*raw);
                raw->start();
            } catch (const std::exception& e) {
                log_warn(std::string("Object view lights: ") + e.what());
            }
        }
    }
