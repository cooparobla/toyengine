// editor/app/ui/ui_canvas.inl -- included inside EditorApp's class body.
//
// The UI designer: what the viewer becomes when a UI asset (assets/ui/*.yaml) is open.
//
//   Preview     the asset's real canvas, drawn by the game's own UI pass inside a frame the
//               size of a chosen game resolution (Engine::set_scene_ui_placement), laid out
//               exactly as the game lays it out there. Wheel zooms about the cursor, middle /
//               Space drag pans, Home fits, F frames the selection.
//   Design      click to select (Alt-click cycles through what's under the cursor, drag on
//               empty space box-selects); the rect gizmo moves / resizes / anchors / pivots /
//               rotates (rect_gizmo.h) with snapping guides; arrows nudge (Shift: 10 px);
//               Shift+A or the Widgets palette adds widgets (or drag them onto the frame);
//               inside a layout group a drag reorders instead.
//   Interact    the canvas runs: hover, press, sliders, tabs, dialogs -- with every named
//               signal echoed to the Console. Leaving it rebuilds from the document.
//
// Document edits all go through SceneDocument (undo, save); rect edits use ChangeScope::Rect,
// which patches the live RectTransform in place so a drag stays at full frame rate.

    // =================================================================================
    // State
    // =================================================================================

    struct UiResolution { const char* label; int w, h; };
    static const std::vector<UiResolution>& ui_resolutions_() {
        static const std::vector<UiResolution> r = {
            {"1920 x 1080  (16:9)", 1920, 1080}, {"1280 x 720  (16:9)", 1280, 720}, {"2560 x 1440  (16:9)", 2560, 1440},
            {"3840 x 2160  (4K)", 3840, 2160}, {"2560 x 1080  (21:9)", 2560, 1080}, {"1280 x 800  (16:10 handheld)", 1280, 800},
            {"1024 x 768  (4:3)", 1024, 768}, {"1080 x 1920  (portrait)", 1080, 1920},
        };
        return r;
    }

    glm::vec2 ui_res_{1920.0f, 1080.0f};   ///< Preview resolution (game framebuffer pixels).
    float ui_zoom_ = 0.0f;                  ///< Framebuffer pixels per game pixel; 0 = fit (recomputed).
    bool ui_fit_ = true;
    glm::vec2 ui_pan_{0.0f};
    bool ui_interact_ = false;
    bool ui_outlines_ = false, ui_safe_area_ = false, ui_snap_ = true;   // outlines: opt-in guides (they square off a rounded theme)
    float ui_grid_ = 1.0f;
    int ui_backdrop_ = 0;                   ///< 0 dark, 1 light, 2 the 3D view (transparent)
    imm::Box ui_frame_{};                   ///< The preview frame, editor pixels.
    RectGizmo ui_gizmo_;
    bool ui_panning_ = false;
    std::vector<ui::SnapLine> ui_guides_;
    std::string ui_palette_filter_;
    int ui_reorder_index_ = -1;
    ObjectId ui_reorder_parent_ = 0;
    std::vector<coopa::event::ScopedConnection> ui_signal_log_;
    ObjectId ui_hover_ = 0;
    std::vector<ObjectId> ui_cycle_;
    size_t ui_cycle_i_ = 0;
    glm::vec2 ui_cycle_at_{-1e6f};
    bool ui_select_pending_ = false, ui_marquee_ = false;
    glm::vec2 ui_press_{0.0f};
    std::map<ObjectId, ui::RectParams> ui_drag_others_;   ///< Other selected objects' params when a move began.
    glm::vec2 ui_menu_at_{0.0f};
    std::string ui_new_name_ = "new_ui";
    coopa::scene::SceneObject* ui_wrapper_ = nullptr;   ///< Preview canvas around a canvas-less widget asset.

public:
    // --- queries for tests ---
    bool ui_mode() const { return active_type_ == AssetType::UI; }
    imm::Box ui_frame() const { return ui_frame_; }
    bool ui_interacting() const { return ui_interact_; }
    void set_ui_interact(bool on) { ui_set_interact_(on); }
    glm::vec2 ui_resolution() const { return ui_res_; }
    void set_ui_resolution(glm::vec2 r) { ui_res_ = r; ui_fit_ = true; }
    ui::UiView ui_view() { return ui_view_(); }
    RectGizmo& ui_gizmo() { return ui_gizmo_; }
    /** @brief The document objects under an editor-pixel point, topmost first. */
    std::vector<ObjectId> ui_pick(glm::vec2 editor_px) { return ui_hits_(editor_px); }
    /** @brief Adds palette entry `id` under `parent` (0: the container at `at`, else the root). */
    ObjectId ui_add_widget(const std::string& id, ObjectId parent = 0, std::optional<glm::vec2> at = std::nullopt) {
        return ui_add_widget_(id, parent, at);
    }
    /** @brief The live rect of a document object (canvas space). */
    std::optional<ui::Rect> ui_live_rect(ObjectId id) {
        auto* live = sync_.live(id);
        auto* rt = live ? live->get_component<coopa::ui::RectTransform>() : nullptr;
        if (!rt) return std::nullopt;
        if (auto* c = live->get_component<coopa::ui::CanvasComponent>()) return c->root_rect();
        return rt->rect();
    }

    /** @brief Opens a UI asset (ui/*.yaml): the object document in the UI designer. */
    bool open_ui_asset(const fs::path& path) {
        stop();
        try {
            doc_.load(path);
        } catch (const std::exception& e) {
            log_error(std::string("Open UI failed: ") + e.what());
            return false;
        }
        if (!doc_.is_object_asset()) { log_error(project_.relative(path) + " is not a UI asset (no `object:`)"); return false; }
        ui_set_interact_(false);
        set_view_(AssetType::UI);
        asset_tab_ = AssetType::UI;
        active_path_ = path;
        sync_.fallback_path = path;
        ui_load_editor_extras_();
        rebuild_scene_();
        if (ObjectId root = doc_.object_root()) doc_.select(root);
        ui_fit_ = true;
        log_info("Opened UI " + project_.relative(path) + "  (Shift A adds widgets; Tab: Interact)");
        return true;
    }

    /** @brief The folder UI templates ship in (editor/templates/ui). */
    static fs::path ui_templates_dir() { return fs::path(ROOT_DIR) / "editor" / "templates" / "ui"; }

    /** @brief Template ids ("hud", "pause_menu", ...) available to New UI. */
    static std::vector<std::string> ui_templates() {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(ui_templates_dir(), ec)) {
            if (e.is_regular_file() && e.path().extension() == ".yaml") out.push_back(e.path().stem().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    /**
     * @brief Creates assets/ui/<name>.yaml -- from a template ("blank" = an empty canvas,
     *        "widget" = a canvas-less reusable piece) -- and opens it. Templates bring their
     *        themes along (into ui/themes/) when the project lacks them.
     */
    bool new_ui_asset(const std::string& name, const std::string& template_id = "blank") {
        std::string base = name.empty() ? std::string("new_ui") : name;
        for (char& c : base) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
        const std::string n = unique_asset_name_("ui", base);
        const fs::path path = project_.assets() / "ui" / (n + ".yaml");
        Node doc;
        if (template_id == "blank" || template_id == "widget") {
            Node obj = Node::mapping();
            obj["name"] = Node(n);
            Node comps = Node::sequence();
            if (template_id == "blank") {
                comps.as_seq().push_back(ui::palette_detail::stretch());
                Node canvas = Node::mapping();
                canvas["type"] = Node(std::string("Canvas"));
                canvas["mode"] = Node(std::string("ScaleWithScreenSize"));
                canvas["reference_resolution"] = ui::palette_detail::v2(1920, 1080);
                canvas["match_width_or_height"] = make_float(0.5);
                comps.as_seq().push_back(canvas);
            } else {
                comps.as_seq().push_back(ui::palette_detail::centered(240, 80));
            }
            obj["components"] = comps;
            obj["children"] = Node::sequence();
            doc = Node::mapping();
            doc["format"] = Node(std::string("toyengine-object"));
            doc["object"] = obj;
        } else {
            try {
                doc = coopa::yaml::load_document(ui_templates_dir() / (template_id + ".yaml"));
            } catch (const std::exception& e) {
                log_error("UI template '" + template_id + "': " + e.what());
                return false;
            }
            if (doc.contains("object") && doc["object"].is_mapping()) doc["object"]["name"] = Node(n);
            ui_install_template_themes_();
        }
        try {
            fs::create_directories(path.parent_path());
            coopa::yaml::save_document(path, doc);
            project_.refresh();
        } catch (const std::exception& e) {
            log_error(std::string("Create UI failed: ") + e.what());
            return false;
        }
        return open_ui_asset(path);
    }

    /**
     * @brief Places a UI asset in the open scene as `prefab: ui/<name>` -- a HUD or menu that is
     *        part of the scene from the start. In a UI asset it nests (a reusable widget).
     */
    ObjectId place_ui_asset(const std::string& ui_rel, ObjectId parent = 0) {
        if (playing() || asset_view_()) return 0;
        const std::string stem = fs::path(ui_rel).stem().string();
        if (doc_.is_object_asset() && active_path_.stem().string() == stem) { log_warn("A UI asset can't contain itself"); return 0; }
        std::string ref = fs::path(ui_rel).replace_extension().generic_string();
        Node obj = Node::mapping();
        obj["name"] = Node(doc_.unique_name(stem, parent));
        obj["prefab"] = Node(ref);
        obj["components"] = Node::sequence();
        const ObjectId id = doc_.add_object(obj, parent, -1, "Place " + stem);
        after_structure_change_(id);
        return id;
    }

private:
    // =================================================================================
    // Document helpers
    // =================================================================================

    /** @brief Preview settings kept in the asset's `ui_editor:` block (ignored by the game). */
    void ui_load_editor_extras_() {
        const Node& ex = doc_.object_extras();
        if (ex.is_mapping() && ex.contains("ui_editor")) {
            const Node& u = ex.at("ui_editor");
            ui_res_.x = std::max(64.0f, get_float(u, "preview_width", ui_res_.x));
            ui_res_.y = std::max(64.0f, get_float(u, "preview_height", ui_res_.y));
        } else if (const Node* root = doc_.find(doc_.object_root())) {
            // No preview chosen yet: the canvas's own reference resolution.
            const int ci = doc_.find_component(doc_.object_root(), "Canvas");
            if (ci >= 0) {
                const Node& c = root->at("components").as_seq()[static_cast<size_t>(ci)];
                const glm::vec2 rr = ui::read_vec2(c, "reference_resolution", glm::vec2(1920, 1080));
                if (rr.x >= 64 && rr.y >= 64) ui_res_ = rr;
            }
        }
    }
    void ui_store_editor_extras_() {
        Node u = Node::mapping();
        u["preview_width"] = Node(static_cast<int64_t>(ui_res_.x));
        u["preview_height"] = Node(static_cast<int64_t>(ui_res_.y));
        doc_.set_object_extra("ui_editor", u);
    }

    /** @brief Copies the templates' themes (ui/themes/) and the fonts they name (fonts/) into
     *         the project, each only when missing. */
    void ui_install_template_themes_() {
        std::error_code ec;
        auto copy_dir = [&](const fs::path& src, const fs::path& dst) {
            if (!fs::is_directory(src, ec)) return;
            fs::create_directories(dst, ec);
            for (const auto& e : fs::directory_iterator(src, ec)) {
                if (!e.is_regular_file()) continue;
                const fs::path to = dst / e.path().filename();
                if (!fs::exists(to, ec)) fs::copy_file(e.path(), to, ec);
            }
        };
        copy_dir(ui_templates_dir() / "themes", project_.assets() / "ui" / "themes");
        copy_dir(ui_templates_dir() / "fonts", project_.assets() / "fonts");
        project_.refresh();
    }

    coopa::scene::SceneObject* ui_root_live_() { return sync_.live(doc_.object_root()); }

    /** @brief The canvas the preview shows: the asset root's, or the wrapper around a widget. */
    coopa::ui::CanvasComponent* ui_canvas_() {
        if (auto* root = ui_root_live_()) {
            if (auto* c = root->get_component<coopa::ui::CanvasComponent>()) return c;
        }
        return ui_wrapper_ ? ui_wrapper_->get_component<coopa::ui::CanvasComponent>() : nullptr;
    }

    /** @brief A component node of `id` by type (null if absent). */
    const Node* ui_comp_(ObjectId id, const std::string& type) const {
        const int ci = doc_.find_component(id, type);
        return ci < 0 ? nullptr : &doc_.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
    }

    /** @brief Leaf widgets: dropping onto one adds next to it, not inside. */
    bool ui_is_leaf_(ObjectId id) const {
        static const char* leaves[] = {"Text", "ThemedText", "ThemedButton", "Button", "Slider", "ProgressBar", "Toggle",
                                       "StatBar", "Hotbar", "ItemGrid", "MessageLog", "PromptBar", "SettingRow",
                                       "InventoryGrid", "ComboBox", "NumberField", "SpinBox", "Image"};
        for (const char* t : leaves) if (doc_.find_component(id, t) >= 0) return true;
        return false;
    }

    // =================================================================================
    // Live scene: backdrop and the preview wrapper
    // =================================================================================

    /**
     * @brief After a rebuild in the UI designer: a backdrop canvas (behind everything, inside
     *        the frame) and -- for a widget asset with no Canvas of its own -- a preview canvas
     *        the widget is moved under. Both are live-only, never saved.
     */
    void ui_after_rebuild_() {
        ui_wrapper_ = nullptr;
        coopa::scene::Scene* scene = sync_.scene();
        if (!scene || active_type_ != AssetType::UI) return;
        // Every UI file restyles from its own Theme (or the default): what the previous file
        // made active must not leak into this one.
        auto make = [&](const std::string& name, int sort, bool image) {
            auto obj = std::make_unique<coopa::scene::SceneObject>(name);
            auto* rt = obj->add_component<coopa::ui::RectTransform>();
            rt->anchor_preset(coopa::ui::AnchorPreset::StretchAll);
            rt->set_size_delta({0.0f, 0.0f});
            rt->hittable = false;
            auto* c = obj->add_component<coopa::ui::CanvasComponent>();
            c->sort_order = sort;
            if (image) {
                auto* bg = std::make_unique<coopa::scene::SceneObject>("Fill").release();
                std::unique_ptr<coopa::scene::SceneObject> fill(bg);
                auto* frt = fill->add_component<coopa::ui::RectTransform>();
                frt->anchor_preset(coopa::ui::AnchorPreset::StretchAll);
                frt->set_size_delta({0.0f, 0.0f});
                frt->hittable = false;
                fill->add_component<coopa::ui::Image>()->raycast_target = false;
                obj->add_child(std::move(fill));
            }
            coopa::scene::SceneObject* raw = scene->add_root_object(std::move(obj));
            scene->adopt(*raw);
            raw->start();
            return raw;
        };
        make("__ui_backdrop", -1000000, true);
        coopa::scene::SceneObject* root = ui_root_live_();
        if (root && !root->get_component<coopa::ui::CanvasComponent>()) {
            // Move the widget under a preview canvas (ownership moves out of the root list).
            coopa::scene::SceneObject* wrapper = make("__ui_preview_canvas", 0, false);
            auto& roots = scene->root_objects();
            for (auto it = roots.begin(); it != roots.end(); ++it) {
                if (it->get() != root) continue;
                std::unique_ptr<coopa::scene::SceneObject> owned = std::move(*it);
                roots.erase(it);
                wrapper->add_child(std::move(owned));
                break;
            }
            if (auto* c = wrapper->get_component<coopa::ui::CanvasComponent>()) {
                c->scaler.mode = coopa::ui::ScaleMode::ScaleWithScreenSize;
                c->scaler.reference_resolution = ui_res_;
            }
            ui_wrapper_ = wrapper;
        }
        ui_update_backdrop_();
    }

    void ui_update_backdrop_() {
        coopa::scene::Scene* scene = sync_.scene();
        if (!scene) return;
        auto* bd = scene->find_object("__ui_backdrop");
        if (!bd) return;
        bd->set_active(ui_backdrop_ != 2);
        if (auto* fill = bd->find_child("Fill")) {
            if (auto* img = fill->get_component<coopa::ui::Image>()) {
                img->color = ui_backdrop_ == 1 ? glm::vec4(0.78f, 0.79f, 0.81f, 1.0f) : glm::vec4(0.16f, 0.17f, 0.19f, 1.0f);
            }
        }
    }

    // =================================================================================
    // View: frame placement, mapping, zoom / pan
    // =================================================================================

    /** @brief The frame for the current zoom / pan inside the viewport (editor pixels). */
    void ui_layout_frame_() {
        const imm::Box vp = viewport_box_;
        const float s = std::max(1.0f, ui_scale_);
        if (ui_fit_ || ui_zoom_ <= 0.0f) {
            const float margin = 28.0f;
            ui_zoom_ = std::max(0.02f, std::min((vp.w - 2 * margin) * s / ui_res_.x, (vp.h - 2 * margin) * s / ui_res_.y));
            ui_pan_ = glm::vec2(0.0f);
            ui_fit_ = false;
        }
        const glm::vec2 size = ui_res_ * ui_zoom_ / s;
        const glm::vec2 c = vp.center() + ui_pan_;
        ui_frame_ = imm::Box{std::round(c.x - size.x * 0.5f), std::round(c.y - size.y * 0.5f), std::round(size.x), std::round(size.y)};
    }

    /** @brief Where the engine draws the previewed canvas (framebuffer pixels). */
    std::optional<core::Engine::ScreenUiPlacement> ui_preview_placement_() {
        if (ui_frame_.empty()) return std::nullopt;
        const float s = std::max(1.0f, ui_scale_);
        core::Engine::ScreenUiPlacement p;
        p.rect.x = static_cast<int32_t>(std::lround(ui_frame_.x * s));
        p.rect.y = static_cast<int32_t>(std::lround(ui_frame_.y * s));
        p.rect.w = static_cast<uint32_t>(std::max(1L, std::lround(ui_frame_.w * s)));
        p.rect.h = static_cast<uint32_t>(std::max(1L, std::lround(ui_frame_.h * s)));
        p.zoom = static_cast<float>(p.rect.w) / std::max(1.0f, ui_res_.x);
        p.input = ui_interact_;
        return p;
    }

    /** @brief The frame <-> canvas mapping (canvas space from the live canvas's root rect). */
    ui::UiView ui_view_() {
        ui::UiView v;
        v.origin = {ui_frame_.x, ui_frame_.y};
        glm::vec2 canvas = ui_res_;
        if (auto* c = ui_canvas_()) if (c->root_rect().size().x > 0.0f) canvas = c->root_rect().size();
        v.canvas_size = canvas;
        v.scale = canvas.x > 0.0f ? ui_frame_.w / canvas.x : 1.0f;
        return v;
    }

    void ui_zoom_at_(glm::vec2 m, float factor) {
        const float s = std::max(1.0f, ui_scale_);
        const float old_zoom = ui_zoom_;
        ui_zoom_ = std::clamp(ui_zoom_ * factor, 0.02f, 16.0f);
        const glm::vec2 old_size = ui_res_ * old_zoom / s, new_size = ui_res_ * ui_zoom_ / s;
        const glm::vec2 centre = viewport_box_.center() + ui_pan_;
        const glm::vec2 rel = (m - centre) / old_size;
        ui_pan_ = (m - rel * new_size) - viewport_box_.center();
    }

    /** @brief Zooms so the selection (else the whole canvas) fills the viewport. */
    void ui_frame_selection_() {
        const ui::UiView v = ui_view_();
        bool any = false;
        ui::Rect bb{glm::vec2(1e30f), glm::vec2(-1e30f)};
        for (ObjectId id : doc_.selection()) {
            if (id == doc_.object_root()) continue;
            if (auto r = ui_live_rect(id)) { bb.min = glm::min(bb.min, r->min); bb.max = glm::max(bb.max, r->max); any = true; }
        }
        if (!any) { ui_fit_ = true; return; }
        const float s = std::max(1.0f, ui_scale_);
        const glm::vec2 sz = glm::max(bb.size(), glm::vec2(8.0f));
        // Editor pixels the selection should span, and the zoom that gives it.
        const float want = std::min((viewport_box_.w * 0.6f) / sz.x, (viewport_box_.h * 0.6f) / sz.y);   // editor px per canvas px
        const float canvas_per_res = v.canvas_size.x / std::max(1.0f, ui_res_.x);
        ui_zoom_ = std::clamp(want * canvas_per_res * s, 0.02f, 16.0f);
        const glm::vec2 frame_size = ui_res_ * ui_zoom_ / s;
        const float scale = frame_size.x / v.canvas_size.x;
        const glm::vec2 c = bb.center();
        // Put the selection's centre in the viewport centre.
        const glm::vec2 c_in_frame{c.x * scale, (v.canvas_size.y - c.y) * scale};
        ui_pan_ = frame_size * 0.5f - c_in_frame;
    }

    // =================================================================================
    // Picking
    // =================================================================================

    /** @brief Document objects under `p` (editor px), topmost first (draw order + z_order). */
    std::vector<ObjectId> ui_hits_(glm::vec2 p) {
        std::vector<ObjectId> out;
        coopa::scene::SceneObject* root = ui_root_live_();
        if (!root || !ui_frame_.contains(p)) return out;
        const ui::UiView v = ui_view_();
        const glm::vec2 cp = v.to_canvas(p);
        std::unordered_map<const coopa::scene::SceneObject*, ObjectId> ids;
        for (const auto& [id, obj] : sync_.live_objects()) ids[obj] = id;
        struct Hit { const coopa::scene::SceneObject* obj; int z; int order; };
        std::vector<Hit> hits;
        int order = 0;
        std::function<void(const coopa::scene::SceneObject&, int)> walk = [&](const coopa::scene::SceneObject& o, int z) {
            if (!o.active()) return;
            const auto* rt = o.get_component<coopa::ui::RectTransform>();
            const int zz = z + (rt ? rt->z_order : 0);
            if (rt && &o != root && !o.get_component<coopa::ui::CanvasComponent>() && ui::point_in_quad(rt->world_corners(), cp)) {
                hits.push_back({&o, zz, order});
            }
            ++order;
            for (const auto& c : o.children()) walk(*c, zz);
        };
        walk(*root, 0);
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.z != b.z ? a.z > b.z : a.order > b.order; });
        for (const Hit& h : hits) {
            ObjectId id = 0;
            for (const coopa::scene::SceneObject* o = h.obj; o && !id; o = o->parent()) {
                auto it = ids.find(o);
                if (it != ids.end()) id = it->second;
            }
            if (id && id != doc_.object_root() && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
        }
        return out;
    }

    /** @brief The object a drop at `p` goes into: the deepest non-leaf object hit, else the root. */
    ObjectId ui_container_at_(glm::vec2 p) {
        for (ObjectId id : ui_hits_(p)) {
            if (!ui_is_leaf_(id)) return id;
            if (auto parent = doc_.parent_of(id)) return *parent ? *parent : doc_.object_root();
        }
        return doc_.object_root();
    }

    // =================================================================================
    // Edits
    // =================================================================================

    /** @brief Writes rect params (+ rotation) into `id`'s RectTransform (adding one if missing). */
    void ui_write_rect_(ObjectId id, const ui::RectParams& p, std::optional<float> rotation, const std::string& label,
                        const std::string& merge) {
        apply_(doc_.edit(label, [&](Node&) -> Change {
            Node* list = doc_.components(id);
            if (!list) return {};
            int ci = doc_.find_component(id, "RectTransform");
            if (ci < 0) {
                Node rt = Node::mapping();
                rt["type"] = Node(std::string("RectTransform"));
                list->as_seq().insert(list->as_seq().begin(), rt);
                ci = 0;
            }
            Node& rt = list->as_seq()[static_cast<size_t>(ci)];
            ui::write_rect_params(rt, p);
            if (rotation) {
                if (std::abs(*rotation) < 1e-4f) erase_key(rt, "rotation");
                else rt["rotation"] = make_float(*rotation);
            }
            return {ChangeScope::Rect, id};
        }, merge));
    }

    /** @brief In a layout group: the element's preferred size (LayoutElement), which the group honours. */
    void ui_write_preferred_size_(ObjectId id, glm::vec2 size, const std::string& merge) {
        apply_(doc_.edit("Resize UI", [&](Node&) -> Change {
            Node* list = doc_.components(id);
            if (!list) return {};
            int ci = doc_.find_component(id, "LayoutElement");
            if (ci < 0) {
                Node le = Node::mapping();
                le["type"] = Node(std::string("LayoutElement"));
                list->as_seq().push_back(le);
                ci = static_cast<int>(list->size()) - 1;
            }
            Node& le = list->as_seq()[static_cast<size_t>(ci)];
            le["preferred_size"] = ui::palette_detail::v2(std::round(size.x), std::round(size.y));
            return {ChangeScope::Object, id};
        }, merge));
    }

    /** @brief The parsed RectTransform params of a document object. */
    ui::RectParams ui_doc_params_(ObjectId id) {
        if (const Node* rt = ui_comp_(id, "RectTransform")) return ui::parse_rect_block(*rt).params();
        return ui::RectParams{};
    }

    /** @brief Adds a palette entry; see ui_add_widget(). */
    ObjectId ui_add_widget_(const std::string& entry_id, ObjectId parent, std::optional<glm::vec2> at) {
        const ui::PaletteEntry* e = ui::find_palette_entry(entry_id);
        if (!e || playing()) return 0;
        if (e->component) {   // a reactor: onto the selection
            const ObjectId target = doc_.primary();
            if (!target) { log_warn("Select an element first: " + e->label + " is added to it"); return 0; }
            apply_(doc_.add_component(target, e->make()));
            return target;
        }
        if (parent == 0) parent = at ? ui_container_at_(*at) : (doc_.primary() && !ui_is_leaf_(doc_.primary()) ? doc_.primary() : doc_.object_root());
        if (ui_is_leaf_(parent)) parent = doc_.parent_of(parent).value_or(doc_.object_root());
        if (parent == 0) parent = doc_.object_root();
        Node obj = e->make();
        obj["name"] = Node(doc_.unique_name(get_string(obj, "name", "Element"), parent));
        // Dropped at a point: place it there (unless a layout group will place it).
        auto* parent_live = sync_.live(parent);
        const bool laid_out = parent_live && (parent_live->get_component<coopa::ui::LayoutGroupBase>() != nullptr);
        if (at && !laid_out && parent_live) {
            const auto* prt = parent_live->get_component<coopa::ui::RectTransform>();
            ui::Rect parent_rect = prt ? prt->rect() : ui::Rect{};
            if (auto* c = parent_live->get_component<coopa::ui::CanvasComponent>()) parent_rect = c->root_rect();
            for (auto& c : obj["components"].as_seq()) {
                if (component_type(c) != "RectTransform") continue;
                ui::RectParams p = ui::parse_rect_block(c).params();
                const glm::vec2 cp = ui_view_().to_canvas(*at);
                ui::Rect r = coopa::ui::resolve_rect(parent_rect, p);
                const glm::vec2 shift = cp - r.center();
                r.min += shift; r.max += shift;
                ui::set_rect(parent_rect, p, r);
                p.anchored_position = glm::round(p.anchored_position);
                ui::write_rect_params(c, p);
            }
        }
        const ObjectId id = doc_.add_object(obj, parent, -1, "Add " + e->label);
        after_structure_change_(id);
        return id;
    }

    /** @brief Arrow-key nudge of every selected element. */
    void ui_nudge_(glm::vec2 d) {
        for (ObjectId id : doc_.selection()) {
            if (id == doc_.object_root()) continue;
            ui::RectParams p = ui_doc_params_(id);
            p.anchored_position += d;
            ui_write_rect_(id, p, std::nullopt, "Nudge", "ui_nudge");
        }
        doc_.end_merge();
    }

    /** @brief Moves `id` one step up (-1) or down (+1) among its siblings: draw order. */
    void ui_restack_(ObjectId id, int dir) {
        const auto parent = doc_.parent_of(id);
        if (!parent) return;
        const Node* list = *parent ? (doc_.find(*parent) ? &doc_.find(*parent)->at("children") : nullptr) : &doc_.root_objects();
        if (!list || !list->is_sequence()) return;
        int idx = -1;
        for (size_t i = 0; i < list->size(); ++i) if (SceneDocument::id_of(list->as_seq()[i]) == id) idx = static_cast<int>(i);
        const int to = idx + dir;
        if (idx < 0 || to < 0 || to >= static_cast<int>(list->size())) return;
        apply_(doc_.reparent(id, *parent, dir > 0 ? to + 1 : to));
    }

    void ui_set_interact_(bool on) {
        if (on == ui_interact_) return;
        ui_interact_ = on;
        ui_gizmo_.cancel();
        ui_signal_log_.clear();
        if (auto* scene = sync_.scene()) {
            if (on) {
                scene->set_simulating(true);
                // Echo every named signal to the Console, so it's clear what game code will hear.
                for (const auto& b : ui_bindings_()) {
                    for (const auto& sig : b.signals) {
                        const std::string name = b.name, signal = sig;
                        ui_signal_log_.emplace_back(scene->events().on(name, signal, [this, name, signal](const coopa::event::EventArgs& a) {
                            std::string extra;
                            if (a.contains("value")) extra = "  value " + a.to_string("value");
                            if (a.contains("index")) extra += "  index " + a.to_string("index");
                            if (a.contains("slot")) extra += "  slot " + a.to_string("slot");
                            log_info("[UI] " + name + "  " + signal + extra);
                        }));
                    }
                }
                log_info("Interact: the UI runs -- signals are echoed here. Esc / Tab returns to Design.");
            } else {
                scene->set_simulating(false);
                queue_rebuild_();   // back to exactly what the document says
            }
        }
    }

    // =================================================================================
    // Bindings: what game code can reach by name
    // =================================================================================

    struct UiBinding {
        std::string name;
        std::string kind;                    ///< "Button", "Slider", "StatBar"...
        std::vector<std::string> signals;    ///< What it publishes on the EventBus.
        std::string api;                     ///< The UiHandle call that drives it.
        ObjectId object = 0;                 ///< The document object it lives on.
    };

    std::vector<UiBinding> ui_bindings_() {
        std::vector<UiBinding> out;
        auto add = [&](std::string name, std::string kind, std::vector<std::string> sigs, std::string api, ObjectId id) {
            out.push_back({std::move(name), std::move(kind), std::move(sigs), std::move(api), id});
        };
        doc_.for_each_object([&](const Node& o, int) {
            const ObjectId id = SceneDocument::id_of(o);
            const std::string name = get_string(o, "name");
            if (!o.contains("components")) return;
            for (const auto& c : o.at("components").as_seq()) {
                const std::string t = component_type(c);
                auto item_names = [&](const char* key) {
                    std::vector<std::string> names;
                    if (c.contains(key) && c.at(key).is_sequence()) {
                        for (const auto& it : c.at(key).as_seq()) {
                            std::string n = it.is_mapping() ? get_string(it, "name") : std::string();
                            const std::string l = it.is_mapping() ? get_string(it, "label") : it.is_string() ? it.get_value<std::string>() : "";
                            if (n.empty()) n = coopa::ui::detail::name_from_label(l.empty() ? std::string("Button") : l);
                            names.push_back(n);
                        }
                    }
                    return names;
                };
                if (t == "Button" || t == "ThemedButton") add(name, "Button", {"click"}, "on_click(\"" + name + "\", ...)", id);
                else if (t == "Slider" || t == "NumberField" || t == "SpinBox") add(name, t, {"value_changed"}, "get<float>(\"" + name + "\")", id);
                else if (t == "Toggle") add(name, t, {"value_changed"}, "get<bool>(\"" + name + "\")", id);
                else if (t == "ComboBox") add(name, t, {"selection_changed"}, "get<int>(\"" + name + "\")", id);
                else if (t == "ProgressBar") add(name, t, {"value_changed"}, "bind_bar(\"" + name + "\", &resource)", id);
                else if (t == "InventoryGrid") add(name, t, {"slot_clicked"}, "bind_inventory(\"" + name + "\", &inv, &db)", id);
                else if (t == "MenuList" || t == "ActionBar") {
                    for (const auto& n : item_names("items")) add(n, "Button (" + t + ")", {"click"}, "on_click(\"" + n + "\", ...)", id);
                } else if (t == "Dialog") {
                    for (const auto& n : item_names("buttons")) add(n, "Button (Dialog)", {"click"}, "on_click(\"" + n + "\", ...)", id);
                    add(name, "Dialog", {"opened", "closed"}, "show(\"" + name + "\") / hide(...)", id);
                    if (get_bool(c, "close_button", false)) add("Close", "Button (Dialog)", {"click"}, "on_click(\"" + name + "/Close\", ...)", id);
                } else if (t == "Window") {
                    if (get_bool(c, "close_button", false)) add("Close", "Button (Window)", {"click"}, "on_click(\"" + name + "/Close\", ...)", id);
                } else if (t == "SettingRow") {
                    const std::string kind = get_string(c, "kind", "slider");
                    const char* getter = kind == "toggle" ? "get<bool>" : kind == "dropdown" ? "get<int>" : kind == "text" || kind == "value" ? "text" : "get<float>";
                    if (kind != "value") add(name, "Setting (" + kind + ")", {kind == "dropdown" ? "selection_changed" : "value_changed"},
                                             std::string(getter) + "(\"" + name + "\")", id);
                    else add(name, "Setting (value)", {}, "set_text(\"" + name + "\", ...)", id);
                } else if (t == "StatBar") add(name, "StatBar", {"value_changed"}, "bind_bar(\"" + name + "\", &resource)", id);
                else if (t == "Hotbar" || t == "ItemGrid") add(name, t, {"slot_clicked"}, "bind_inventory(\"" + name + "\", &inv, &db)", id);
                else if (t == "TabView") add(name, t, {"tab_changed"}, "on(\"" + name + "\", \"tab_changed\", ...)", id);
                else if (t == "MessageLog") add(name, t, {}, "log(\"" + name + "\", \"text\")", id);
                else if (t == "Text" || t == "ThemedText") add(name, "Text", {}, "set_text(\"" + name + "\", ...)", id);
            }
        });
        return out;
    }

    // =================================================================================
    // The view
    // =================================================================================

    void draw_ui_view_(imm::Context& ctx, const imm::Box& area) {
        const imm::Box hb = area_header_(ctx, area);
        draw_ui_header_(ctx, hb);
        viewport_box_ = imm::Box{area.x, hb.bottom(), area.w, std::max(1.0f, area.h - hb.h)};
        ui_layout_frame_();
        ui_update_backdrop_();
        ctx.push_clip(viewport_box_);
        // Around the frame: the editor's own backdrop (inside it the game's canvas draws).
        const glm::vec4 around = ui_backdrop_ == 1 ? glm::vec4(0.55f, 0.56f, 0.58f, 1.0f) : glm::vec4(0.10f, 0.10f, 0.11f, 1.0f);
        if (ui_backdrop_ != 2) fill_around_(ctx, viewport_box_, ui_frame_.intersect(viewport_box_), around);
        handle_ui_input_(ctx);
        draw_ui_overlays_(ctx);
        ctx.pop_clip();
        draw_ui_popups_(ctx);
        // Drops: palette widgets onto the frame, UI assets as nested prefabs.
        if (!ui_interact_) {
            if (auto w = ctx.drop_target("ui_widget", ui_frame_)) ui_add_widget_(*w, 0, ctx.mouse());
            if (auto a = ctx.drop_target("asset", ui_frame_); a && asset_type_of_(*a) == AssetType::UI) {
                const ObjectId parent = ui_container_at_(ctx.mouse());
                place_ui_asset(*a, parent);
            }
        }
    }

    void draw_ui_header_(imm::Context& ctx, const imm::Box& hb) {
        using I = imm::Icon;
        const float bh = hb.h - 6;
        float x = hb.x + 6;
        if (ui_interact_) ctx.fill_rounded(hb, et_.chrome.play_tint, 6, imm::Context::kTop);
        // Design / Interact.
        int mode = ui_interact_ ? 1 : 0;
        if (ctx.icon_group("ui_mode", {{I::SelectBox, "Design\nSelect, move and resize elements (Tab toggles)"},
                                       {I::Play, "Interact\nRun the UI: buttons, sliders and tabs respond, signals print to the Console"}},
                           &mode, imm::Box{x, hb.y + 3, bh * 2, bh}, bh)) {
            ui_set_interact_(mode == 1);
        }
        x += bh * 2 + 10;
        // Preview resolution.
        std::vector<std::string> res_names;
        int res_idx = -1;
        for (size_t i = 0; i < ui_resolutions_().size(); ++i) {
            const auto& r = ui_resolutions_()[i];
            res_names.push_back(r.label);
            if (static_cast<int>(ui_res_.x) == r.w && static_cast<int>(ui_res_.y) == r.h) res_idx = static_cast<int>(i);
        }
        if (res_idx < 0) { res_names.push_back(std::to_string(int(ui_res_.x)) + " x " + std::to_string(int(ui_res_.y)) + "  (custom)"); res_idx = static_cast<int>(res_names.size()) - 1; }
        const float rw = 190;
        if (ctx.combo_box("ui_res", {x, hb.y + 3, rw, bh}, &res_idx, res_names) && res_idx < static_cast<int>(ui_resolutions_().size())) {
            ui_res_ = {static_cast<float>(ui_resolutions_()[static_cast<size_t>(res_idx)].w), static_cast<float>(ui_resolutions_()[static_cast<size_t>(res_idx)].h)};
            ui_fit_ = true;
            ui_store_editor_extras_();
            if (ui_wrapper_) queue_rebuild_();
        }
        ctx.tooltip("Preview Resolution\nThe game window size the canvas is laid out for -- check a HUD at every size you ship");
        x += rw + 8;
        // Zoom.
        char zbuf[32];
        std::snprintf(zbuf, sizeof zbuf, "%d%%", static_cast<int>(std::lround(ui_zoom_ * 100.0f)));
        std::vector<std::string> zooms = {zbuf, "Fit", "25%", "50%", "100%", "200%"};
        int zi = 0;
        const float zw = 70;
        if (ctx.combo_box("ui_zoom", {x, hb.y + 3, zw, bh}, &zi, zooms) && zi > 0) {
            if (zi == 1) ui_fit_ = true;
            else { ui_zoom_ = zi == 2 ? 0.25f : zi == 3 ? 0.5f : zi == 4 ? 1.0f : 2.0f; ui_pan_ = glm::vec2(0.0f); }
        }
        ctx.tooltip("Zoom\nWheel zooms about the cursor; Home fits; F frames the selection");
        x += zw + 10;
        // Toggles.
        auto toggle = [&](const char* id, I icon, bool* v, const char* tip) {
            if (ctx.icon_button(id, icon, tip, *v, bh, imm::Context::kAll, imm::Box{x, hb.y + 3, bh, bh}, true)) *v = !*v;
            x += bh + 3;
        };
        toggle("ui_snap", I::Snap, &ui_snap_, "Snapping\nTo the parent's and siblings' edges and centres, and the pixel grid (Ctrl inverts)");
        toggle("ui_outlines", I::Overlays, &ui_outlines_, "Element Outlines\nFaint outlines around every element");
        toggle("ui_safe", I::Monitor, &ui_safe_area_, "Safe Area\nTV / handheld safe frames (90% action, 80% title)");
        x += 6;
        std::vector<std::string> grids = {"Grid 1", "Grid 2", "Grid 4", "Grid 8", "Grid 16"};
        int gi = ui_grid_ >= 16 ? 4 : ui_grid_ >= 8 ? 3 : ui_grid_ >= 4 ? 2 : ui_grid_ >= 2 ? 1 : 0;
        if (ctx.combo_box("ui_grid", {x, hb.y + 3, 74, bh}, &gi, grids)) ui_grid_ = static_cast<float>(1 << gi);
        ctx.tooltip("Snap Grid\nPositions and sizes round to this many canvas pixels while snapping");
        x += 80;
        std::vector<std::string> bgs = {"Dark", "Light", "3D View"};
        if (ctx.combo_box("ui_bg", {x, hb.y + 3, 80, bh}, &ui_backdrop_, bgs)) ui_update_backdrop_();
        ctx.tooltip("Preview Background\nWhat shows behind the canvas (the game draws its 3D view there)");
        x += 86;
        // Add (palette menu).
        if (!ui_interact_) {
            const imm::Box ab{x, hb.y + 3, 64, bh};
            bool hov = false, held = false;
            if (ctx.invisible_button("ui_add_btn", ab, &hov, &held)) { ui_menu_at_ = ui_frame_.center(); ctx.open_popup("ui_add_menu", glm::vec2(ab.x, ab.bottom() + 2)); }
            ctx.fill_rounded(ab, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.icon(I::Plus, {ab.x + 4, ab.y + 3, bh - 6, bh - 6}, ctx.style.text);
            ctx.text_in({ab.x + bh, ab.y, ab.w - bh, ab.h}, "Add", ctx.style.text, 0.0f);
            ctx.tooltip("Add Widget\nShift A over the preview; or drag one from the Widgets palette");
        }
    }

    void handle_ui_input_(imm::Context& ctx) {
        const auto& in = ctx.input();
        const glm::vec2 m = ctx.mouse();
        const bool shift = has(in.mods, Mods::Shift);
        const bool alt = has(in.mods, Mods::Alt);
        const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
        const bool hovered = ctx.is_hovered(viewport_box_) && !ctx.popup_hovered();
        viewport_hovered_ = hovered;
        const double pinch = trackpad::take_magnify() + std::exchange(pinch_override_, 0.0);

        // --- navigation: wheel / pinch zoom, middle or Space drag pan ---
        if (hovered) {
            if (pinch != 0.0) ui_zoom_at_(m, static_cast<float>(1.0 + pinch));
            if (in.scroll.y != 0.0f) {
                const auto& tp = trackpad::state();
                const bool trackpad = trackpad_override_ ? *trackpad_override_ : tp.scroll_is_trackpad;
                if (trackpad && !ctrl) ui_pan_ += in.scroll * 12.0f;   // two-finger swipe pans, pinch zooms
                else ui_zoom_at_(m, std::pow(1.12f, in.scroll.y));
            }
        }
        const bool space = in.key_down && in.key_down(Key::Space) && !ctx.wants_keyboard();
        if (hovered && (in.pressed[2] || (space && in.pressed[0]))) ui_panning_ = true;
        if (ui_panning_) {
            if (!in.down[2] && !in.down[0]) ui_panning_ = false;
            else ui_pan_ += in.mouse_delta;
            return;
        }
        if (ui_interact_) {
            if (hovered && !ctx.wants_keyboard() && (ctx.shortcut(Key::Escape) || ctx.shortcut(Key::Tab))) ui_set_interact_(false);
            return;
        }

        // --- the gizmo on the active element ---
        ui_guides_.clear();
        bool gizmo_took = false;
        const ObjectId sel = doc_.primary();
        RectGizmoTarget target;
        const bool has_target = sel && sel != doc_.object_root() && ui_gizmo_target_(sel, target);
        const ui::UiView view = ui_view_();
        auto run_gizmo = [&](const RectGizmoTarget& t, bool pressed) {
            RectGizmoInput gi;
            gi.mouse = m;
            gi.pressed = pressed;
            gi.down = in.down[0];
            gi.released = in.released[0];
            gi.shift = shift; gi.alt = alt; gi.ctrl = ctrl;
            gi.hover_ok = hovered;
            RectGizmoSettings gs;
            gs.snap = ui_snap_;
            gs.grid = ui_grid_;
            gs.lines = ui_snap_lines_(sel, t.parent);
            const bool starting = !ui_gizmo_.dragging();
            const RectGizmoResult r = ui_gizmo_.update(view, t, gi, gs);
            if (r.started && starting) {
                ui_drag_others_.clear();
                for (ObjectId o : doc_.selection()) if (o != sel && o != doc_.object_root()) ui_drag_others_[o] = ui_doc_params_(o);
            }
            ui_guides_ = r.guides;
            if (r.reorder) ui_update_reorder_(sel, m);
            if (r.changed) {
                const std::string merge = "uirect:" + std::to_string(sel);
                if (t.in_layout && r.handle != RectHandle::Body) {
                    ui_write_preferred_size_(sel, r.rect.size(), merge);
                } else if (!t.in_layout) {
                    ui_write_rect_(sel, r.params, r.rotation, r.handle == RectHandle::Body ? "Move UI" : "Edit UI Rect", merge);
                    if (r.handle == RectHandle::Body) {
                        const glm::vec2 d = r.params.anchored_position - t.params.anchored_position;
                        for (auto& [o, p0] : ui_drag_others_) {
                            ui::RectParams p = ui_doc_params_(o);
                            p.anchored_position += d;
                            ui_write_rect_(o, p, std::nullopt, "Move UI", merge);
                        }
                    }
                }
            }
            if (r.finished) {
                if (ui_reorder_index_ >= 0) {
                    apply_(doc_.reparent(sel, ui_reorder_parent_, ui_reorder_index_));
                    ui_reorder_index_ = -1;
                }
                doc_.end_merge();
            }
            return r;
        };
        if (has_target && (ui_gizmo_.dragging() || ui_gizmo_.hit(view, target, m) != RectHandle::None)) {
            const bool over_handle = ui_gizmo_.hit(view, target, m) != RectHandle::None && ui_gizmo_.hit(view, target, m) != RectHandle::Body;
            // A plain click on the body of the selection still re-picks (Alt cycles); handles always drag.
            if (ui_gizmo_.dragging() || over_handle || (in.pressed[0] && !alt && !shift)) {
                const RectGizmoResult r = run_gizmo(target, in.pressed[0] && hovered);
                gizmo_took = r.active || r.started || r.finished || over_handle;
                if (ui_gizmo_.hot() != RectHandle::None) ctx.set_mouse_cursor(RectGizmo::cursor_for(ui_gizmo_.hot()));
            }
        }
        if (gizmo_took) return;

        // --- selection: click (Alt cycles), Shift adds, drag on empty space box-selects ---
        if (hovered && in.pressed[0]) { ui_press_ = m; ui_select_pending_ = true; ui_marquee_ = false; }
        if (ui_select_pending_ && in.down[0] && glm::distance(m, ui_press_) > 5.0f) {
            const auto hits = ui_hits_(ui_press_);
            if (hits.empty() || alt) ui_marquee_ = true;
            else {
                // Press on an element and drag: select it and move it in one gesture.
                ui_select_pending_ = false;
                if (!doc_.is_selected(hits.front()) || shift) doc_.select(hits.front(), shift);
                RectGizmoTarget t;
                if (ui_gizmo_target_(hits.front(), t)) {
                    RectGizmoInput gi;
                    gi.mouse = ui_press_; gi.pressed = true; gi.down = true; gi.hover_ok = true;
                    RectGizmoSettings gs;
                    gs.snap = false;
                    ui_gizmo_.update(view, t, gi, gs);   // starts the body drag at the press point
                    ui_drag_others_.clear();
                    for (ObjectId o : doc_.selection()) if (o != hits.front() && o != doc_.object_root()) ui_drag_others_[o] = ui_doc_params_(o);
                }
            }
        }
        if (ui_select_pending_ && in.released[0]) {
            if (ui_marquee_) {
                const glm::vec2 a = glm::min(ui_press_, m), b = glm::max(ui_press_, m);
                if (!shift) doc_.clear_selection();
                for (ObjectId id : doc_.all_ids()) {
                    if (id == doc_.object_root()) continue;
                    auto r = ui_live_rect(id);
                    if (!r) continue;
                    const auto box = view.box_of(*r);
                    if (box[0] >= a.x && box[1] >= a.y && box[0] + box[2] <= b.x && box[1] + box[3] <= b.y) doc_.select(id, true);
                }
            } else {
                const auto hits = ui_hits_(m);
                if (hits.empty()) { if (!shift) doc_.select(doc_.object_root()); }
                else {
                    ObjectId pick = hits.front();
                    if (alt) {
                        // Alt-click again at the same spot: the next element down.
                        if (glm::distance(m, ui_cycle_at_) < 4.0f && ui_cycle_ == hits) ui_cycle_i_ = (ui_cycle_i_ + 1) % hits.size();
                        else ui_cycle_i_ = hits.size() > 1 ? 1 : 0;
                        ui_cycle_ = hits;
                        ui_cycle_at_ = m;
                        pick = hits[ui_cycle_i_];
                    }
                    if (shift) doc_.select(pick, true);
                    else doc_.select(pick);
                }
            }
            ui_select_pending_ = ui_marquee_ = false;
        }
        if (!in.down[0] && !in.released[0]) ui_select_pending_ = ui_marquee_ = false;
        if (ui_marquee_) {
            const imm::Box r{std::min(ui_press_.x, m.x), std::min(ui_press_.y, m.y), std::abs(m.x - ui_press_.x), std::abs(m.y - ui_press_.y)};
            ctx.fill(r, et_.viewport.box_select_fill);
            ctx.outline(r, et_.viewport.box_select_outline);
        }
        const auto hover_hits = hovered ? ui_hits_(m) : std::vector<ObjectId>{};
        ui_hover_ = hover_hits.empty() ? 0 : hover_hits.front();

        // --- context menu, keys ---
        if (hovered && in.pressed[1]) {
            const auto hits = ui_hits_(m);
            if (!hits.empty() && !doc_.is_selected(hits.front())) doc_.select(hits.front());
            ui_menu_at_ = m;
            ctx.open_popup("ui_ctx", m);
        }
        if (hovered && !ctx.wants_keyboard() && !ctx.any_popup_open()) ui_keymap_(ctx);
    }

    void ui_keymap_(imm::Context& ctx) {
        const glm::vec2 m = ctx.mouse();
        if (ctx.shortcut(Key::A, Mods::Shift)) { ui_menu_at_ = m; ctx.open_popup("ui_add_menu", m); }
        if (ctx.shortcut(Key::Tab)) ui_set_interact_(true);
        if (ctx.shortcut(Key::Home)) ui_fit_ = true;
        if (ctx.shortcut(Key::F) || ctx.shortcut(Key::Period) || ctx.shortcut(Key::KpDecimal)) ui_frame_selection_();
        if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete) || ctx.shortcut(Key::Backspace)) delete_selected();
        if (ctx.shortcut(Key::D, Mods::Shift) || ctx.shortcut(Key::D, imm::Context::command_mod())) ui_duplicate_();
        if (ctx.shortcut(Key::A)) { doc_.clear_selection(); for (ObjectId id : doc_.all_ids()) if (id != doc_.object_root()) doc_.select(id, true); }
        if (ctx.shortcut(Key::A, Mods::Alt)) doc_.clear_selection();
        if (ctx.shortcut(Key::H)) hide_(doc_.selection());
        if (ctx.shortcut(Key::H, Mods::Alt)) unhide_all_();
        const float step = 1.0f, big = 10.0f;
        if (ctx.shortcut(Key::Left)) ui_nudge_({-step, 0});
        if (ctx.shortcut(Key::Right)) ui_nudge_({step, 0});
        if (ctx.shortcut(Key::Up)) ui_nudge_({0, step});
        if (ctx.shortcut(Key::Down)) ui_nudge_({0, -step});
        if (ctx.shortcut(Key::Left, Mods::Shift)) ui_nudge_({-big, 0});
        if (ctx.shortcut(Key::Right, Mods::Shift)) ui_nudge_({big, 0});
        if (ctx.shortcut(Key::Up, Mods::Shift)) ui_nudge_({0, big});
        if (ctx.shortcut(Key::Down, Mods::Shift)) ui_nudge_({0, -big});
        if (ctx.shortcut(Key::RightBracket) && doc_.primary()) ui_restack_(doc_.primary(), +1);
        if (ctx.shortcut(Key::LeftBracket) && doc_.primary()) ui_restack_(doc_.primary(), -1);
        if (ctx.shortcut(Key::F2) && doc_.primary()) { rename_id_ = doc_.primary(); rename_frames_ = 0; }
    }

    /** @brief Shift+D: duplicates the selection, offset so the copies are visible. */
    void ui_duplicate_() {
        std::vector<ObjectId> ids;
        for (ObjectId id : doc_.selection()) if (id != doc_.object_root()) ids.push_back(id);
        if (ids.empty()) return;
        auto copies = doc_.duplicate_objects(ids);
        doc_.clear_selection();
        for (ObjectId c : copies) {
            ui::RectParams p = ui_doc_params_(c);
            p.anchored_position += glm::vec2(16.0f, -16.0f);
            if (Node* o = doc_.find(c)) {
                Node* list = &ensure_seq(*o, "components");
                for (auto& comp : list->as_seq()) if (component_type(comp) == "RectTransform") ui::write_rect_params(comp, p);
            }
            doc_.select(c, true);
        }
        queue_rebuild_();
    }

    /** @brief The gizmo's view of a document object (false if it has no live rect). */
    bool ui_gizmo_target_(ObjectId id, RectGizmoTarget& t) {
        auto* live = sync_.live(id);
        auto* rt = live ? live->get_component<coopa::ui::RectTransform>() : nullptr;
        if (!rt || live->get_component<coopa::ui::CanvasComponent>() || !live->active()) return false;
        t.rect = rt->rect();
        t.parent = rt->parent_rect();
        t.params = rt->params();
        t.rotation = rt->local_rotation_degrees();
        t.in_layout = false;
        if (auto* parent = live->parent()) {
            const auto* le = live->get_component<coopa::ui::LayoutElement>();
            t.in_layout = parent->get_component<coopa::ui::LayoutGroupBase>() != nullptr && !(le && le->ignore_layout);
        }
        return true;
    }

    /** @brief Snap targets: the parent's edges/centre and every sibling's (not `self`). */
    std::vector<ui::SnapLine> ui_snap_lines_(ObjectId self, const ui::Rect& parent) {
        std::vector<ui::SnapLine> lines;
        ui::add_rect_lines(lines, parent);
        auto* live = sync_.live(self);
        if (!live || !live->parent()) return lines;
        for (const auto& sib : live->parent()->children()) {
            if (sib.get() == live || !sib->active()) continue;
            if (auto* rt = sib->get_component<coopa::ui::RectTransform>()) ui::add_rect_lines(lines, rt->rect());
        }
        return lines;
    }

    /** @brief While dragging inside a layout group: the index the element would drop at. */
    void ui_update_reorder_(ObjectId id, glm::vec2 m) {
        ui_reorder_index_ = -1;
        auto* live = sync_.live(id);
        const auto parent_id = doc_.parent_of(id);
        if (!live || !live->parent() || !parent_id) return;
        const Node* list = *parent_id ? &doc_.find(*parent_id)->at("children") : &doc_.root_objects();
        if (!list->is_sequence()) return;
        const bool horizontal = live->parent()->get_component<coopa::ui::HorizontalLayoutGroup>() != nullptr;
        const glm::vec2 cp = ui_view_().to_canvas(m);
        int index = 0;
        for (const auto& sib : list->as_seq()) {
            const ObjectId sid = SceneDocument::id_of(sib);
            auto r = ui_live_rect(sid);
            if (!r) continue;
            const bool before = horizontal ? cp.x > r->center().x : cp.y < r->center().y;
            if (before) index = static_cast<int>(&sib - &list->as_seq()[0]) + 1;
        }
        ui_reorder_index_ = index;
        ui_reorder_parent_ = *parent_id;
    }

    void draw_ui_overlays_(imm::Context& ctx) {
        const ui::UiView v = ui_view_();
        // The canvas frame.
        ctx.outline(ui_frame_, imm::with_alpha(ctx.style.text, 0.35f));
        if (ui_safe_area_) {
            for (float k : {0.9f, 0.8f}) {
                const imm::Box sb{ui_frame_.x + ui_frame_.w * (1 - k) * 0.5f, ui_frame_.y + ui_frame_.h * (1 - k) * 0.5f, ui_frame_.w * k, ui_frame_.h * k};
                ctx.outline(sb, glm::vec4(1.0f, 0.85f, 0.3f, k > 0.85f ? 0.5f : 0.3f));
            }
        }
        if (ui_interact_) return;
        // Faint outlines on every element.
        if (ui_outlines_) {
            for (ObjectId id : doc_.all_ids()) {
                if (id == doc_.object_root() || hidden_.count(id)) continue;
                auto* live = sync_.live(id);
                if (!live || !live->active()) continue;
                auto r = ui_live_rect(id);
                if (!r) continue;
                const auto b = v.box_of(*r);
                ctx.outline({b[0], b[1], b[2], b[3]}, imm::with_alpha(ctx.style.text, 0.10f));
            }
        }
        // Hover.
        if (ui_hover_ && !doc_.is_selected(ui_hover_)) {
            if (auto r = ui_live_rect(ui_hover_)) {
                const auto b = v.box_of(*r);
                ctx.outline({b[0], b[1], b[2], b[3]}, imm::with_alpha(ctx.style.accent, 0.55f));
            }
        }
        // Selection: outlines, the parent's rect, the gizmo on the active element.
        for (ObjectId id : doc_.selection()) {
            if (id == doc_.object_root() || id == doc_.primary()) continue;
            if (auto r = ui_live_rect(id)) {
                const auto b = v.box_of(*r);
                ctx.outline({b[0], b[1], b[2], b[3]}, ctx.style.accent, 1.5f);
            }
        }
        RectGizmoTarget t;
        const ObjectId sel = doc_.primary();
        if (sel && sel != doc_.object_root() && ui_gizmo_target_(sel, t)) {
            const auto pb = v.box_of(t.parent);
            ctx.outline({pb[0], pb[1], pb[2], pb[3]}, imm::with_alpha(ctx.style.text, 0.3f));
            ui_gizmo_.draw(ctx, v, t, ctx.style.accent);
            // Size readout under the element.
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d x %d", static_cast<int>(std::lround(t.rect.size().x)), static_cast<int>(std::lround(t.rect.size().y)));
            const auto b = v.box_of(t.rect);
            const float tw = ctx.text_width(buf) + 10;
            const imm::Box lb{b[0] + b[2] * 0.5f - tw * 0.5f, b[1] + b[3] + 6, tw, 18};
            ctx.fill_rounded(lb, et_.viewport.modal_backdrop);
            ctx.text_in(lb, buf, ctx.style.text, 0.0f, true);
            if (t.in_layout) {
                const imm::Box lk{b[0] + 2, b[1] - 18, 16, 16};
                ctx.icon(imm::Icon::Lock, lk, ctx.style.text_dim);
                ctx.tooltip("Positioned by its parent's layout group: drag to reorder, resize sets its preferred size");
            }
        }
        // Snapping guides.
        const glm::vec4 guide{1.0f, 0.25f, 0.85f, 0.9f};
        for (const auto& g : ui_guides_) {
            if (g.axis == 0) {
                const glm::vec2 a = v.to_editor({g.value, 0.0f}), b = v.to_editor({g.value, v.canvas_size.y});
                ctx.line(a, b, guide);
            } else {
                const glm::vec2 a = v.to_editor({0.0f, g.value}), b = v.to_editor({v.canvas_size.x, g.value});
                ctx.line(a, b, guide);
            }
        }
        // Reorder insertion line.
        if (ui_reorder_index_ >= 0 && sel) {
            if (auto* live = sync_.live(sel); live && live->parent()) {
                if (auto* prt = live->parent()->get_component<coopa::ui::RectTransform>()) {
                    const bool horizontal = live->parent()->get_component<coopa::ui::HorizontalLayoutGroup>() != nullptr;
                    const glm::vec2 cp = v.to_canvas(ctx.mouse());
                    const ui::Rect pr = prt->rect();
                    const glm::vec2 a = horizontal ? v.to_editor({cp.x, pr.min.y}) : v.to_editor({pr.min.x, cp.y});
                    const glm::vec2 b = horizontal ? v.to_editor({cp.x, pr.max.y}) : v.to_editor({pr.max.x, cp.y});
                    ctx.line(a, b, ctx.style.accent, 3.0f);
                }
            }
        }
        // Empty canvas hint.
        if (doc_.all_ids().size() <= 1) {
            const std::string h = "Empty canvas -- Shift A adds widgets, or drag them in from the Widgets palette";
            const float w = ctx.text_width(h) + 20;
            const imm::Box b{ui_frame_.center().x - w * 0.5f, ui_frame_.center().y - 14, w, 28};
            ctx.fill_rounded(b, et_.viewport.modal_backdrop);
            ctx.text_in(b, h, ctx.style.text, 0.0f, true);
        }
    }

    void draw_ui_add_menu_items_(imm::Context& ctx, glm::vec2 at) {
        for (const auto& cat : ui::palette_categories()) {
            if (!ctx.begin_menu(cat)) continue;
            for (const auto& e : ui::palette()) {
                if (e.category != cat) continue;
                if (ctx.menu_item(e.label, "", nullptr, !e.component || doc_.primary() != 0, e.icon)) {
                    ui_add_widget_(e.id, 0, ui_frame_.contains(at) ? std::optional<glm::vec2>(at) : std::nullopt);
                }
                ctx.tooltip(e.label + "\n" + e.tip);
            }
            ctx.end_menu();
        }
        if (ctx.begin_menu("UI Asset")) {
            const auto assets = list_assets_(AssetType::UI);
            for (const auto& rel : assets) {
                if (ctx.menu_item(fs::path(rel).stem().string(), "", nullptr, true, imm::Icon::Link)) {
                    place_ui_asset(rel, ui_frame_.contains(at) ? ui_container_at_(at) : doc_.object_root());
                }
            }
            if (assets.empty()) ctx.label_dim("No UI assets yet");
            ctx.end_menu();
        }
    }

    void draw_ui_popups_(imm::Context& ctx) {
        using I = imm::Icon;
        if (ctx.begin_popup("ui_add_menu", 190)) {
            ctx.label_dim("Add Widget");
            draw_ui_add_menu_items_(ctx, ui_menu_at_);
            ctx.end_popup();
        }
        if (ctx.begin_popup("ui_ctx", 220)) {
            const ObjectId id = doc_.primary();
            const bool any = id && id != doc_.object_root();
            if (ctx.begin_menu("Add Widget", true, I::Plus)) { draw_ui_add_menu_items_(ctx, ui_menu_at_); ctx.end_menu(); }
            ctx.menu_separator();
            if (ctx.menu_item("Duplicate", "Shift D", nullptr, any, I::Duplicate)) ui_duplicate_();
            if (ctx.menu_item("Delete", "X", nullptr, any, I::Trash)) delete_selected();
            if (ctx.menu_item("Rename", "F2", nullptr, any)) { rename_id_ = id; rename_frames_ = 0; }
            ctx.menu_separator();
            if (ctx.menu_item("Bring Forward", "]", nullptr, any)) ui_restack_(id, +1);
            if (ctx.menu_item("Send Backward", "[", nullptr, any)) ui_restack_(id, -1);
            if (ctx.menu_item("Select Parent", "", nullptr, any)) { if (auto p = doc_.parent_of(id)) doc_.select(*p ? *p : doc_.object_root()); }
            ctx.menu_separator();
            if (ctx.menu_item("Frame Selected", "F", nullptr, any, I::Zoom)) ui_frame_selection_();
            if (ctx.menu_item("Fit Canvas", "Home")) ui_fit_ = true;
            ctx.end_popup();
        }
    }

    // =================================================================================
    // The Widgets palette (left panel, under the asset list)
    // =================================================================================

    void draw_ui_palette_(imm::Context& ctx, const imm::Box& area) {
        using I = imm::Icon;
        const imm::Box title{area.x + 8, area.y + 2, area.w - 16, 20};
        ctx.text_in(title, "Widgets", ctx.style.text, 0.0f);
        const imm::Box search{area.x + 6, title.bottom() + 2, area.w - 12, 22};
        ctx.input_text_box("ui_palette_filter", search, &ui_palette_filter_, "    Search widgets");
        if (ui_palette_filter_.empty()) ctx.icon(I::Search, {search.x + 4, search.y + 4, 14, 14}, ctx.style.text_disabled);
        const imm::Box body{area.x, search.bottom() + 4, area.w, area.bottom() - search.bottom() - 4};
        ctx.begin_region("ui_palette", body, true);
        std::string f = ui_palette_filter_;
        std::transform(f.begin(), f.end(), f.begin(), ::tolower);
        for (const auto& cat : ui::palette_categories()) {
            bool header = false;
            for (const auto& e : ui::palette()) {
                if (e.category != cat) continue;
                std::string l = e.label + " " + e.tip;
                std::transform(l.begin(), l.end(), l.begin(), ::tolower);
                if (!f.empty() && l.find(f) == std::string::npos) continue;
                if (!header) { ctx.label_dim(cat); header = true; }
                ctx.push_id(e.id);
                if (ctx.selectable(e.label, false, 0, e.icon) && !ui_interact_) ui_add_widget_(e.id, 0, std::nullopt);
                ctx.tooltip(e.label + "\n" + e.tip + (e.component ? "\nClick: add to the selected element" :
                                                                   "\nClick: add to the selection (or the canvas); drag: drop it where you want it"));
                if (!e.component) ctx.drag_source("ui_widget", e.id, e.label);
                ctx.pop_id();
            }
        }
        ctx.end_region();
    }

    // =================================================================================
    // Properties: Canvas, the element's Rect Transform, Bindings
    // =================================================================================

    void draw_ui_canvas_props_(imm::Context& ctx) {
        using I = imm::Icon;
        InspectorEnv env = inspector_env_();
        const ObjectId root = doc_.object_root();
        if (ctx.collapsing_header("Preview", true, nullptr, I::Monitor)) {
            int w = static_cast<int>(ui_res_.x), h = static_cast<int>(ui_res_.y);
            bool ch = ctx.drag_int("Width", &w, 1.0f, 64, 16384);
            ch |= ctx.drag_int("Height", &h, 1.0f, 64, 16384);
            if (ch) { ui_res_ = {static_cast<float>(w), static_cast<float>(h)}; ui_fit_ = true; ui_store_editor_extras_(); }
            ctx.tooltip("Preview Resolution\nSaved in the asset's ui_editor block (the game ignores it)");
            ctx.property_bool("Safe Area", &ui_safe_area_);
            ctx.property_bool("Element Outlines", &ui_outlines_);
            ctx.property_bool("Snapping", &ui_snap_);
        }
        if (!root) return;
        const int ci = doc_.find_component(root, "Canvas");
        if (ci < 0) {
            ctx.label_dim("This is a widget: it has no Canvas of its own, and is placed inside other UI");
            ctx.label_dim("(Shift A > UI Asset in another UI, or drag it onto one).");
        } else if (ctx.collapsing_header("Canvas", true, nullptr, I::UiCanvas)) {
            Node comp = doc_.find(root)->at("components").as_seq()[static_cast<size_t>(ci)];
            const Node before = comp;
            ctx.indent(6);
            EditResult r = draw_component(ctx, comp, env);
            ctx.unindent(6);
            if (r.changed && !(comp == before)) apply_(doc_.set_component(root, ci, comp, "Edit Canvas", r.active ? "ui_canvas" : std::string()));
            if (r.finished) doc_.end_merge();
        }
        if (ctx.collapsing_header("Theme", true, nullptr, I::Palette)) {
            const int ti = doc_.find_component(root, "Theme");
            Node comp = ti >= 0 ? doc_.find(root)->at("components").as_seq()[static_cast<size_t>(ti)] : Node::mapping();
            if (ti < 0) comp["type"] = Node(std::string("Theme"));
            FieldDesc f = f_asset("source", "ui/themes", ".yaml");
            f.label = "Theme";
            const Node before = comp;
            EditResult r = draw_field(ctx, f, comp, env);
            ctx.tooltip("Theme\nColours, fonts and sizes of every themed widget (Window, MenuList, StatBar...) in this UI");
            if (r.changed && !(comp == before)) {
                if (ti >= 0 && !comp.contains("source")) apply_(doc_.remove_component(root, ti));
                else if (ti >= 0) apply_(doc_.set_component(root, ti, comp, "Change Theme"));
                else if (comp.contains("source")) apply_(doc_.add_component(root, comp));
                queue_rebuild_();
            }
            if (ctx.button("New Theme", -1, true, I::Plus)) ui_new_theme_();
            ctx.tooltip("New Theme\nCopies the current theme (or the default one) to ui/themes/ and uses it");
            const std::string src = ti >= 0 ? get_string(comp, "source") : std::string();
            if (!src.empty() && ctx.button("Edit Theme", -1, true, I::Palette)) open_asset(AssetType::Theme, src);
            ctx.tooltip("Edit Theme\nOpens the theme in the Theme tab: every colour, font and size, previewed here");
        }
    }

    /** @brief Copies the in-use theme (or the template default) into ui/themes/ and selects it. */
    void ui_new_theme_() {
        ui_install_template_themes_();
        const ObjectId root = doc_.object_root();
        const int ti = doc_.find_component(root, "Theme");
        std::string from = ti >= 0 ? get_string(doc_.find(root)->at("components").as_seq()[static_cast<size_t>(ti)], "source") : std::string();
        if (from.empty()) from = "ui/themes/default.yaml";
        const std::string n = unique_asset_name_("ui/themes", fs::path(doc_.scene_name()).stem().string() + "_theme");
        const fs::path dst = project_.assets() / "ui" / "themes" / (n + ".yaml");
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        if (fs::exists(project_.absolute(from), ec)) fs::copy_file(project_.absolute(from), dst, ec);
        else coopa::yaml::save_document(dst, Node::mapping());
        project_.refresh();
        Node comp = Node::mapping();
        comp["type"] = Node(std::string("Theme"));
        comp["source"] = Node("ui/themes/" + n + ".yaml");
        if (ti >= 0) apply_(doc_.set_component(root, ti, comp, "New Theme"));
        else apply_(doc_.add_component(root, comp));
        log_info("Theme ui/themes/" + n + ".yaml created");
        open_theme(dst);
    }

    /**
     * @brief The Object tab for a UI element: name, the Rect Transform (anchor presets, then
     *        position / size or the four edges for a stretched axis), visibility.
     */
    void draw_ui_object_props_(imm::Context& ctx, ObjectId id) {
        using I = imm::Icon;
        const Node* obj = doc_.find(id);
        if (!obj) return;
        {
            imm::Box row = ctx.next_box(ctx.style.row_height + 2);
            auto [icon, tint] = object_icon_(*obj, ctx.style);
            ctx.icon(icon, {row.x + 2, row.y + 3, row.h - 6, row.h - 6}, tint);
            std::string name = get_string(*obj, "name", "Element");
            if (ctx.input_text_box("objname", {row.x + row.h + 2, row.y, row.w - row.h - 2, row.h}, &name) && !name.empty()) {
                apply_(doc_.set_object_key(id, "name", Node(name), "Rename"));
            }
            ctx.tooltip("Name\nGame code finds this element by its name (Bindings tab)");
        }
        const bool is_canvas = doc_.find_component(id, "Canvas") >= 0;
        if (is_canvas) {
            ctx.label_dim("The canvas fills the game window; its look is set in the Canvas tab.");
        } else if (ctx.collapsing_header("Rect Transform", true, nullptr, I::UiAnchor)) {
            draw_ui_rect_section_(ctx, id);
        }
        if (ctx.collapsing_header("Visibility", true, nullptr, I::Eye)) {
            bool shown = !hidden_.count(id);
            if (ctx.property_bool("Show in Editor", &shown)) { if (shown) { hidden_.erase(id); if (auto* l = sync_.live(id)) l->set_active(get_bool(*doc_.find(id), "active", true)); } else hide_({id}); }
            bool enabled = get_bool(*doc_.find(id), "active", true);
            if (ctx.property_bool("Visible in Game", &enabled)) apply_(doc_.set_object_key(id, "active", Node(enabled), "Toggle Active"));
            ctx.tooltip("Visible in Game\nStarts hidden when off -- show it from game code (UiHandle::show) or a Show On Signal reactor");
        }
        if (obj->contains("prefab")) ctx.label_dim("Instance of " + get_string(*obj, "prefab") + " (edits here override it)");
    }

    void draw_ui_rect_section_(imm::Context& ctx, ObjectId id) {
        const Node* blk = ui_comp_(id, "RectTransform");
        coopa::ui::RectTransform rtp = blk ? ui::parse_rect_block(*blk) : coopa::ui::RectTransform{};
        ui::RectParams p = rtp.params();
        float rot = rtp.local_rotation_degrees();
        glm::vec2 scale = rtp.local_scale();
        int z = rtp.z_order;
        bool hittable = rtp.hittable;
        ui::Rect parent{};
        RectGizmoTarget t;
        const bool live = ui_gizmo_target_(id, t);
        if (live) parent = t.parent;
        if (live && t.in_layout) ctx.label_dim("A layout group places this element: size it with a LayoutElement (resize handles do).");

        // Anchor preset button: a picture of the current anchors, opening the 4x4 picker.
        {
            imm::Box row = ctx.next_box(52);
            const imm::Box pb{row.x + 2, row.y + 2, 48, 48};
            bool hov = false, held = false;
            if (ctx.invisible_button("anchor_preset", pb, &hov, &held)) ctx.open_popup("ui_anchor_picker", glm::vec2(pb.x, pb.bottom() + 2));
            draw_anchor_glyph_(ctx, pb, p.anchor_min, p.anchor_max, hov);
            const ui::AnchorPresetInfo* cur = ui::matching_preset(p);
            ctx.text_in({pb.right() + 8, row.y + 4, row.w - 60, 20}, cur ? cur->name : "Custom anchors", ctx.style.text, 0.0f);
            ctx.text_in({pb.right() + 8, row.y + 24, row.w - 60, 20}, "Click to pick (Shift: pivot too, Alt: snap position)", ctx.style.text_dim, 0.0f);
            ctx.tooltip("Anchor Presets\nWhere the element hangs on its parent when the screen size changes.\n"
                        "Shift also sets the pivot; Alt also moves it onto the anchors.");
        }
        if (ctx.begin_popup("ui_anchor_picker", 236)) {
            const auto& presets = ui::anchor_presets();
            const bool shift = has(ctx.input().mods, Mods::Shift), alt = has(ctx.input().mods, Mods::Alt);
            for (int r = 0; r < 4; ++r) {
                imm::Box row = ctx.next_box(52);
                for (int c = 0; c < 4; ++c) {
                    const auto& pr = presets[static_cast<size_t>(r * 4 + c)];
                    const imm::Box cb{row.x + c * 56.0f, row.y, 52, 52};
                    bool hov = false, held = false;
                    ctx.push_id(pr.name);
                    if (ctx.invisible_button("cell", cb, &hov, &held)) {
                        ui::RectParams np = p;
                        ui::apply_anchor_preset(parent, np, pr, shift, alt);
                        ui_write_rect_(id, np, std::nullopt, std::string("Anchor ") + pr.name, {});
                        ctx.close_current_popup();
                    }
                    draw_anchor_glyph_(ctx, cb, pr.amin, pr.amax, hov);
                    ctx.tooltip(std::string(pr.name) + "\nShift: set the pivot too.  Alt: snap the position to the anchors too.");
                    ctx.pop_id();
                }
            }
            ctx.label_dim(shift || alt ? (std::string(shift ? "+ pivot " : "") + (alt ? "+ position" : "")) : "Hold Shift / Alt for more");
            ctx.end_popup();
        }

        bool changed = false, active = false, finished = false;
        auto track = [&](bool ch) { changed |= ch; active |= ctx.last_group_active() || ctx.last_active(); finished |= ctx.last_deactivated(); };
        glm::vec2 omin = coopa::ui::offset_min(ui::Rect{}, p), omax = coopa::ui::offset_max(ui::Rect{}, p);
        const bool sx = std::abs(p.anchor_max.x - p.anchor_min.x) > 1e-5f, sy = std::abs(p.anchor_max.y - p.anchor_min.y) > 1e-5f;
        float row1[2] = {sx ? omin.x : p.anchored_position.x, sy ? -omax.y : p.anchored_position.y};
        float row2[2] = {sx ? -omax.x : p.size_delta.x, sy ? omin.y : p.size_delta.y};
        const bool c1 = ctx.drag_floatn(sx || sy ? (std::string(sx ? "Left" : "Pos X") + " / " + (sy ? "Top" : "Pos Y")) : "Position", row1, 2, 1.0f, "%.0f");
        track(c1);
        const bool c2 = ctx.drag_floatn(sx || sy ? (std::string(sx ? "Right" : "Width") + " / " + (sy ? "Bottom" : "Height")) : "Size", row2, 2, 1.0f, "%.0f");
        track(c2);
        if (c1 || c2) {
            if (sx) { omin.x = row1[0]; omax.x = -row2[0]; } else { p.anchored_position.x = row1[0]; p.size_delta.x = row2[0]; }
            if (sy) { omax.y = -row1[1]; omin.y = row2[1]; } else { p.anchored_position.y = row1[1]; p.size_delta.y = row2[1]; }
            if (sx || sy) {
                ui::RectParams q = p;
                coopa::ui::set_offsets(ui::Rect{}, q, omin, omax);
                if (sx) { p.anchored_position.x = q.anchored_position.x; p.size_delta.x = q.size_delta.x; }
                if (sy) { p.anchored_position.y = q.anchored_position.y; p.size_delta.y = q.size_delta.y; }
            }
        }
        float amin[2] = {p.anchor_min.x, p.anchor_min.y}, amax[2] = {p.anchor_max.x, p.anchor_max.y}, piv[2] = {p.pivot.x, p.pivot.y};
        if (ctx.drag_floatn("Anchor Min", amin, 2, 0.005f, "%.3f")) { track(true); ui::set_anchors_keep_rect(parent, p, {amin[0], amin[1]}, p.anchor_max); }
        else track(false);
        if (ctx.drag_floatn("Anchor Max", amax, 2, 0.005f, "%.3f")) { track(true); ui::set_anchors_keep_rect(parent, p, p.anchor_min, {amax[0], amax[1]}); }
        else track(false);
        if (ctx.drag_floatn("Pivot", piv, 2, 0.005f, "%.3f")) { track(true); ui::set_pivot_keep_rect(parent, p, {piv[0], piv[1]}); }
        else track(false);
        const bool cr = ctx.drag_float("Rotation", &rot, 0.5f, -360.0f, 360.0f, "%.1f");
        track(cr);
        float sc[2] = {scale.x, scale.y};
        const bool cs = ctx.drag_floatn("Scale", sc, 2, 0.01f, "%.2f");
        track(cs);
        if (changed) {
            ui_write_rect_(id, p, rot, "Edit Rect Transform", active ? "ui_rect_props:" + std::to_string(id) : std::string());
            if (cs) {
                apply_(doc_.edit("Scale UI", [&](Node&) -> Change {
                    const int ci = doc_.find_component(id, "RectTransform");
                    if (ci < 0) return {};
                    Node& rt = doc_.components(id)->as_seq()[static_cast<size_t>(ci)];
                    rt["scale"] = ui::palette_detail::v2(sc[0], sc[1]);
                    return {ChangeScope::Rect, id};
                }, active ? "ui_rect_props:" + std::to_string(id) : std::string()));
            }
        }
        if (finished) doc_.end_merge();
        if (ctx.drag_int("Z Order", &z, 0.1f, -10000, 10000) || ctx.property_bool("Hittable", &hittable)) {
            apply_(doc_.edit("Edit Rect Transform", [&](Node&) -> Change {
                const int ci = doc_.find_component(id, "RectTransform");
                if (ci < 0) return {};
                Node& rt = doc_.components(id)->as_seq()[static_cast<size_t>(ci)];
                if (z == 0) erase_key(rt, "z_order"); else rt["z_order"] = Node(static_cast<int64_t>(z));
                if (hittable) erase_key(rt, "hittable"); else rt["hittable"] = Node(false);
                return {ChangeScope::Rect, id};
            }));
        }
        ctx.tooltip("Z Order draws above (or below) later siblings; Hittable off lets clicks through");
    }

    /** @brief A small picture of an anchor setup: the parent box, the anchor rect, its corners. */
    void draw_anchor_glyph_(imm::Context& ctx, const imm::Box& b, glm::vec2 amin, glm::vec2 amax, bool hot) {
        ctx.fill_rounded(b, hot ? ctx.style.button_hover : ctx.style.button);
        const imm::Box in{b.x + 8, b.y + 8, b.w - 16, b.h - 16};
        ctx.outline(in, imm::with_alpha(ctx.style.text, 0.5f));
        auto at = [&](glm::vec2 a) { return glm::vec2(in.x + in.w * a.x, in.y + in.h * (1.0f - a.y)); };
        const glm::vec2 p0 = at(amin), p1 = at(amax);
        const imm::Box ab{std::min(p0.x, p1.x) - 1, std::min(p0.y, p1.y) - 1, std::abs(p1.x - p0.x) + 2, std::abs(p1.y - p0.y) + 2};
        ctx.fill(ab, imm::with_alpha(ctx.style.accent, 0.35f));
        ctx.outline(ab, ctx.style.accent);
        for (glm::vec2 c : {p0, p1, glm::vec2(p0.x, p1.y), glm::vec2(p1.x, p0.y)}) ctx.circle(c, 2.5f, glm::vec4(1.0f, 0.4f, 0.4f, 1.0f));
    }

    void draw_ui_bindings_(imm::Context& ctx) {
        using I = imm::Icon;
        const auto bindings = ui_bindings_();
        std::map<std::string, int> counts;
        for (const auto& b : bindings) ++counts[b.name];
        if (ctx.collapsing_header("Bindings", true, nullptr, I::Link)) {
            ctx.label_dim("What game code reaches by name, through coopa::ui::UiHandle.");
            if (bindings.empty()) ctx.label_dim("No buttons, sliders or other named widgets yet.");
            for (const auto& b : bindings) {
                ctx.push_id(b.name + b.kind);
                imm::Box row = ctx.next_box(ctx.style.row_height);
                const bool dup = counts[b.name] > 1 && b.name != "Close";
                bool hov = false, held = false;
                if (ctx.invisible_button("bind_row", row, &hov, &held)) doc_.select(b.object);
                if (hov) ctx.fill_rounded(row, ctx.style.row_hover);
                ctx.icon(dup ? I::Warning : I::Link, {row.x + 2, row.y + 2, row.h - 4, row.h - 4}, dup ? glm::vec4(1, 0.8f, 0.3f, 1) : ctx.style.text_dim);
                ctx.text_in({row.x + row.h + 2, row.y, row.w * 0.45f, row.h}, b.name, ctx.style.text, 0.0f);
                std::string sig;
                for (const auto& s : b.signals) sig += (sig.empty() ? "" : ", ") + s;
                ctx.text_in({row.x + row.w * 0.48f, row.y, row.w * 0.52f, row.h}, b.kind, ctx.style.text_dim, 0.0f);
                ctx.tooltip(b.name + (dup ? "\nWARNING: more than one element has this name -- game code reaches only the first" : "") +
                            "\n" + b.kind + (sig.empty() ? "" : "\nSignals: " + sig) + "\nui." + b.api);
                ctx.pop_id();
            }
        }
        if (!bindings.empty() && ctx.button("Copy C++ Binding Stub", -1, true, I::Duplicate)) {
            std::string s = "// coopa::ui::UiHandle ui(scene.find_object(\"" + doc_.scene_name() + "\"));\n";
            for (const auto& b : bindings) s += "ui." + b.api + ";   // " + b.kind + "\n";
            if (ctx.input().set_clipboard) ctx.input().set_clipboard(s);
            log_info("Copied " + std::to_string(bindings.size()) + " bindings to the clipboard");
        }
        ctx.tooltip("Copy C++ Binding Stub\nOne UiHandle call per named widget -- paste into a UiController's bind()");
        if (ctx.collapsing_header("No-Code Wiring", false, nullptr, I::Link)) {
            ctx.label_dim("Reactors (Add Widget > Reactors) act on a signal without code:");
            ctx.label_dim("Show On Signal opens a panel when a button is clicked,");
            ctx.label_dim("Color / Text On Signal restyle an element.");
        }
    }
