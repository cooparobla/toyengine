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

    /** @brief The asset type of an assets-relative path (by folder / extension). */
    static AssetType asset_type_of_(const std::string& rel) {
        const fs::path p(rel);
        const std::string ext = p.extension().string();
        const std::string g = p.generic_string();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") return AssetType::Texture;
        if (g.rfind("objects/", 0) == 0) return AssetType::Object;
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
        if (active_type_ != t) return false;
        if (t == AssetType::Mesh) return !mesh_.path.empty() && project_.relative(mesh_.path) == rel;
        return !active_path_.empty() && project_.relative(active_path_) == rel;
    }
    /** @brief Unsaved changes on the open asset (for the list's dot). */
    bool open_asset_dirty_() const {
        switch (active_type_) {
            case AssetType::Scene: case AssetType::Object: return doc_.dirty();
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
        guarded_([this, t, abs] { open_asset_now_(t, abs); });
    }

private:
    void open_asset_now_(AssetType t, const fs::path& abs) {
        switch (t) {
            case AssetType::Scene: open_scene(abs); break;
            case AssetType::Object: open_object_asset(abs); break;
            case AssetType::Mesh: open_mesh(abs); break;
            case AssetType::Material: open_material(abs); break;
            case AssetType::Texture: open_texture(abs); break;
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
        else log_info(item);
    }

public:
    /** @brief Opens an object asset (objects/*.yaml) as a one-object scene. */
    bool open_object_asset(const fs::path& path) {
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
        mr["mesh_path"] = Node(stem);
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
        if (doc_.is_object_asset() && active_path_.stem().string() == stem) { log_warn("An object asset can't contain itself"); return 0; }
        Node obj = Node::mapping();
        obj["name"] = Node(doc_.unique_name(stem));
        obj["prefab"] = Node("objects/" + stem);
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
        // New (+)
        const imm::Box nb{hb.right() - s - 6, hb.y + 4, s, s};
        const bool can_new = asset_tab_ != AssetType::Texture;
        if (ctx.icon_button("asset_new", I::Plus,
                            can_new ? std::string("New ") + info.singular + "\nCreate one in assets/" + info.dir + "/"
                                    : std::string("Import textures by copying image files into assets/textures/"),
                            false, s, imm::Context::kAll, nb) && can_new) {
            if (asset_tab_ == AssetType::Mesh) ctx.open_popup("asset_new_mesh", glm::vec2(nb.x, nb.bottom()));
            else new_asset_(asset_tab_);
        }
        if (ctx.begin_popup("asset_new_mesh", 160)) {
            ctx.label_dim("New Mesh");
            for (const auto& p : primitive_names()) if (ctx.menu_item(p)) guarded_([this, p] { new_mesh(p); save_mesh(); project_.refresh(); });
            ctx.end_popup();
        }
        // Title + search.
        const imm::Box title{area.x + 8, hb.bottom() + 4, area.w - 16, 20};
        const auto items = list_assets_(asset_tab_);
        ctx.text_in(title, std::string(info.label) + "  (" + std::to_string(items.size()) + ")", ctx.style.text, 0.0f);
        const imm::Box search{area.x + 6, title.bottom() + 2, area.w - 12, 22};
        ctx.input_text_box("asset_filter", search, &asset_filter_, "    Search");
        if (asset_filter_.empty()) ctx.icon(I::Search, {search.x + 4, search.y + 4, 14, 14}, ctx.style.text_disabled);
        const imm::Box body{area.x, search.bottom() + 4, area.w, area.bottom() - search.bottom() - 4};
        ctx.begin_region("asset_list", body, true);
        std::string f = asset_filter_;
        std::transform(f.begin(), f.end(), f.begin(), ::tolower);
        size_t shown = 0;
        for (const auto& rel : items) {
            const std::string name = asset_display_name_(asset_tab_, rel);
            std::string ln = rel;
            std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
            if (!f.empty() && ln.find(f) == std::string::npos) continue;
            ++shown;
            ctx.push_id(rel);
            const bool open = asset_is_open_(asset_tab_, rel);
            const std::string label = name + (open && open_asset_dirty_() ? "  *" : "");
            if (ctx.selectable(label, open, 0, info.icon) && !open) open_asset(asset_tab_, rel);
            ctx.tooltip(name + "\nassets/" + rel + (asset_tab_ == AssetType::Object ? "\nDrag into a scene to place an instance" :
                                                    asset_tab_ == AssetType::Material ? "\nDrag onto an object or a mesh slot" :
                                                    asset_tab_ == AssetType::Mesh ? "\nDrag into a scene to add it" : ""));
            ctx.drag_source("asset", rel, name);
            if (ctx.last_clicked(imm::Mouse::Right)) { asset_context_ = rel; ctx.open_popup("asset_ctx"); }
            ctx.pop_id();
        }
        if (shown == 0) {
            ctx.label_dim(items.empty() ? std::string("No ") + info.label + " yet -- press + to create one." : "Nothing matches.");
            if (asset_tab_ == AssetType::Texture && items.empty()) ctx.label_dim("Copy .png files into assets/textures/.");
        }
        if (ctx.begin_popup("asset_ctx", 210)) {
            draw_asset_context_menu_(ctx, asset_tab_, asset_context_);
            ctx.end_popup();
        }
        ctx.end_region();
        draw_asset_modals_(ctx);
    }

    void draw_asset_context_menu_(imm::Context& ctx, AssetType t, const std::string& rel) {
        using I = imm::Icon;
        ctx.label_dim(asset_display_name_(t, rel));
        if (ctx.menu_item("Open", "", nullptr, true, asset_type_info_(t).icon)) open_asset(t, rel);
        if (t == AssetType::Object && ctx.menu_item("Place in Scene", "", nullptr, !asset_view_() && !playing(), I::Plus)) place_object_asset(rel);
        if (t == AssetType::Mesh && ctx.menu_item("Make Object Asset", "", nullptr, true, I::Object)) create_object_asset_from_mesh_(rel);
        if (t == AssetType::Mesh && ctx.menu_item("Add to Scene", "", nullptr, !asset_view_() && !playing(), I::Plus)) add_mesh_to_scene_(rel);
        if (t == AssetType::Material && ctx.menu_item("Assign to Selected", "", nullptr, !asset_view_() && !doc_.selection().empty(), I::Link)) {
            assign_material_(rel);
        }
        ctx.menu_separator();
        if (ctx.menu_item("Duplicate", "", nullptr, true, I::Duplicate)) duplicate_asset_(t, rel);
        if (ctx.menu_item("Rename...", "", nullptr, !asset_is_open_(t, rel))) { asset_rename_ = rel; asset_rename_to_ = fs::path(rel).stem().string(); pending_modal_ = "Rename Asset"; }
        if (ctx.menu_item("Delete...", "", nullptr, !asset_is_open_(t, rel), I::Trash)) { asset_delete_ = rel; pending_modal_ = "Delete Asset"; }
        ctx.menu_separator();
        if (ctx.menu_item("Copy Path", "", nullptr, true) && ctx.input().set_clipboard) ctx.input().set_clipboard(rel);
    }

    void new_asset_(AssetType t) {
        switch (t) {
            case AssetType::Scene: guarded_([this] { new_scene_asset_("scene"); }); break;
            case AssetType::Object: guarded_([this] { new_object_asset("object"); }); break;
            case AssetType::Material: guarded_([this] { create_material("material"); }); break;
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

    void draw_asset_modals_(imm::Context& ctx) {
        if (ctx.begin_modal("Rename Asset", {360, 130})) {
            ctx.label("Rename " + asset_rename_);
            ctx.input_text("New name", &asset_rename_to_);
            if (ctx.button("Rename", 100) && !asset_rename_to_.empty()) {
                const fs::path src = project_.absolute(asset_rename_);
                const fs::path dst = src.parent_path() / (asset_rename_to_ + src.extension().string());
                std::error_code ec;
                if (fs::exists(dst)) log_error(project_.relative(dst) + " already exists");
                else fs::rename(src, dst, ec);
                if (ec) log_error("Rename failed: " + ec.message());
                project_.refresh();
                ctx.close_current_popup();
            }
            ctx.same_line();
            if (ctx.button("Cancel", 80)) ctx.close_current_popup();
            ctx.end_modal();
        }
        if (ctx.begin_modal("Delete Asset", {380, 120})) {
            ctx.label("Delete " + asset_delete_ + "?");
            ctx.label_dim("Scenes and objects referring to it will fail to load it.");
            if (ctx.button("Delete", 100)) {
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

    /** @brief The texture view's material: the image as albedo, fully rough. */
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
        m["texture_albedo"] = Node(project_.relative(active_path_));
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
        if (!doc_.is_object_asset() || !sync_.scene()) return;
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
