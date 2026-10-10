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
    static const std::vector<UiResolution>& ui_resolutions_();

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
    void set_ui_resolution(glm::vec2 r);
    ui::UiView ui_view() { return ui_view_(); }
    RectGizmo& ui_gizmo() { return ui_gizmo_; }
    /** @brief The document objects under an editor-pixel point, topmost first. */
    std::vector<ObjectId> ui_pick(glm::vec2 editor_px) { return ui_hits_(editor_px); }
    /** @brief Adds palette entry `id` under `parent` (0: the container at `at`, else the root). */
    ObjectId ui_add_widget(const std::string& id, ObjectId parent = 0, std::optional<glm::vec2> at = std::nullopt) {
        return ui_add_widget_(id, parent, at);
    }
    /** @brief The live rect of a document object (canvas space). */
    std::optional<ui::Rect> ui_live_rect(ObjectId id);

    /** @brief Opens a UI asset (ui/*.yaml): the object document in the UI designer. */
    bool open_ui_asset(const fs::path& path);

    /** @brief The folder UI templates ship in (editor/templates/ui). */
    static fs::path ui_templates_dir() { return fs::path(ROOT_DIR) / "editor" / "templates" / "ui"; }

    /** @brief Template ids ("hud", "pause_menu", ...) available to New UI. */
    static std::vector<std::string> ui_templates();

    /**
     * @brief Creates assets/ui/<name>.yaml -- from a template ("blank" = an empty canvas,
     *        "widget" = a canvas-less reusable piece) -- and opens it. Templates bring their
     *        themes along (into ui/themes/) when the project lacks them.
     */
    bool new_ui_asset(const std::string& name, const std::string& template_id = "blank");

    /**
     * @brief Places a UI asset in the open scene as `prefab: ui/<name>` -- a HUD or menu that is
     *        part of the scene from the start. In a UI asset it nests (a reusable widget).
     */
    ObjectId place_ui_asset(const std::string& ui_rel, ObjectId parent = 0);

private:
    // =================================================================================
    // Document helpers
    // =================================================================================

    /** @brief Preview settings kept in the asset's `ui_editor:` block (ignored by the game). */
    void ui_load_editor_extras_();
    void ui_store_editor_extras_();

    /** @brief Copies the templates' themes (ui/themes/) and the fonts they name (fonts/) into
     *         the project, each only when missing. */
    void ui_install_template_themes_();

    coopa::scene::SceneObject* ui_root_live_() { return sync_.live(doc_.object_root()); }

    /** @brief The canvas the preview shows: the asset root's, or the wrapper around a widget. */
    coopa::ui::CanvasComponent* ui_canvas_();

    /** @brief A component node of `id` by type (null if absent). */
    const Node* ui_comp_(ObjectId id, const std::string& type) const;

    /** @brief Leaf widgets: dropping onto one adds next to it, not inside. */
    bool ui_is_leaf_(ObjectId id) const;

    // =================================================================================
    // Live scene: backdrop and the preview wrapper
    // =================================================================================

    /**
     * @brief After a rebuild in the UI designer: a backdrop canvas (behind everything, inside
     *        the frame) and -- for a widget asset with no Canvas of its own -- a preview canvas
     *        the widget is moved under. Both are live-only, never saved.
     */
    void ui_after_rebuild_();

    void ui_update_backdrop_();

    // =================================================================================
    // View: frame placement, mapping, zoom / pan
    // =================================================================================

    /** @brief The frame for the current zoom / pan inside the viewport (editor pixels). */
    void ui_layout_frame_();

    /** @brief Where the engine draws the previewed canvas (framebuffer pixels). */
    std::optional<core::Engine::ScreenUiPlacement> ui_preview_placement_();

    /** @brief The frame <-> canvas mapping (canvas space from the live canvas's root rect). */
    ui::UiView ui_view_();

    void ui_zoom_at_(glm::vec2 m, float factor);

    /** @brief Zooms so the selection (else the whole canvas) fills the viewport. */
    void ui_frame_selection_();

    // =================================================================================
    // Picking
    // =================================================================================

    /** @brief Document objects under `p` (editor px), topmost first (draw order + z_order). */
    std::vector<ObjectId> ui_hits_(glm::vec2 p);

    /** @brief The object a drop at `p` goes into: the deepest non-leaf object hit, else the root. */
    ObjectId ui_container_at_(glm::vec2 p);

    // =================================================================================
    // Edits
    // =================================================================================

    /** @brief Writes rect params (+ rotation) into `id`'s RectTransform (adding one if missing). */
    void ui_write_rect_(ObjectId id, const ui::RectParams& p, std::optional<float> rotation, const std::string& label,
                        const std::string& merge);

    /** @brief In a layout group: the element's preferred size (LayoutElement), which the group honours. */
    void ui_write_preferred_size_(ObjectId id, glm::vec2 size, const std::string& merge);

    /** @brief The parsed RectTransform params of a document object. */
    ui::RectParams ui_doc_params_(ObjectId id);

    /** @brief Adds a palette entry; see ui_add_widget(). */
    ObjectId ui_add_widget_(const std::string& entry_id, ObjectId parent, std::optional<glm::vec2> at);

    /** @brief Arrow-key nudge of every selected element. */
    void ui_nudge_(glm::vec2 d);

    /** @brief Moves `id` one step up (-1) or down (+1) among its siblings: draw order. */
    void ui_restack_(ObjectId id, int dir);

    void ui_set_interact_(bool on);

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

    std::vector<UiBinding> ui_bindings_();

    // =================================================================================
    // The view
    // =================================================================================

    void draw_ui_view_(imm::Context& ctx, const imm::Box& area);

    void draw_ui_header_(imm::Context& ctx, const imm::Box& hb);

    void handle_ui_input_(imm::Context& ctx);

    void ui_keymap_(imm::Context& ctx);

    /** @brief Shift+D: duplicates the selection, offset so the copies are visible. */
    void ui_duplicate_();

    /** @brief The gizmo's view of a document object (false if it has no live rect). */
    bool ui_gizmo_target_(ObjectId id, RectGizmoTarget& t);

    /** @brief Snap targets: the parent's edges/centre and every sibling's (not `self`). */
    std::vector<ui::SnapLine> ui_snap_lines_(ObjectId self, const ui::Rect& parent);

    /** @brief While dragging inside a layout group: the index the element would drop at. */
    void ui_update_reorder_(ObjectId id, glm::vec2 m);

    void draw_ui_overlays_(imm::Context& ctx);

    void draw_ui_add_menu_items_(imm::Context& ctx, glm::vec2 at);

    void draw_ui_popups_(imm::Context& ctx);

    // =================================================================================
    // The Widgets palette (left panel, under the asset list)
    // =================================================================================

    void draw_ui_palette_(imm::Context& ctx, const imm::Box& area);

    // =================================================================================
    // Properties: Canvas, the element's Rect Transform, Bindings
    // =================================================================================

    void draw_ui_canvas_props_(imm::Context& ctx);

    /** @brief Copies the in-use theme (or the template default) into ui/themes/ and selects it. */
    void ui_new_theme_();

    /**
     * @brief The Object tab for a UI element: name, the Rect Transform (anchor presets, then
     *        position / size or the four edges for a stretched axis), visibility.
     */
    void draw_ui_object_props_(imm::Context& ctx, ObjectId id);

    void draw_ui_rect_section_(imm::Context& ctx, ObjectId id);

    /** @brief A small picture of an anchor setup: the parent box, the anchor rect, its corners. */
    void draw_anchor_glyph_(imm::Context& ctx, const imm::Box& b, glm::vec2 amin, glm::vec2 amax, bool hot);

    void draw_ui_bindings_(imm::Context& ctx);
