// editor/app/ui/viewport_chrome.inl -- included inside EditorApp's class body.
//
// The 3D Viewport's Blender furniture: the header (mode menu, View / Select / Add / Object
// menus, orientation, snapping, overlays, the four shading buttons), the toolbar column,
// the navigation gizmo, the N sidebar, the view-name overlay text, and Blender-style
// glyphs for lights, cameras and empties.

    // --- layout of the overlay furniture (computed before input so clicks on it never
    //     reach the viewport's select / navigate handlers) ---

    static constexpr float kToolSize = 32.0f;

    imm::Box toolbar_rect_(bool mesh_edit) const;
    float sidebar_w_() const { return show_sidebar_ ? 230.0f : 0.0f; }
    imm::Box sidebar_rect_() const { return {viewport_box_.right() - sidebar_w_(), viewport_box_.y, sidebar_w_(), viewport_box_.h}; }
    imm::Box nav_gizmo_rect_() const {
        return {viewport_box_.right() - sidebar_w_() - 118, viewport_box_.y + 8, 110, 110 + 4 * 30};
    }
    /** @brief True if `p` is over viewport furniture (toolbar, nav gizmo, sidebar). */
    /** @brief Material and texture views only look -- no selection, tools or transforms. */
    bool preview_only_view_() const;

    bool over_viewport_chrome_(glm::vec2 p, bool mesh_edit) const;

    // --- header ---

    void draw_viewport_header_(imm::Context& ctx, const imm::Box& hb, bool mesh_edit);

    /** @brief The proportional editing popover: falloff curve, distance mode, radius. */
    void draw_proportional_popover_(imm::Context& ctx);

    void draw_view_menu_(imm::Context& ctx);

    void draw_select_menu_(imm::Context& ctx, bool mesh_edit);

    void draw_add_menu_items_(imm::Context& ctx);

    void draw_object_menu_(imm::Context& ctx);

    /** @brief Mesh > Mirror (and the Ctrl M menu): flip the selection along an axis through the pivot. */
    void draw_mirror_menu_items_(imm::Context& ctx);

    void draw_mesh_menu_(imm::Context& ctx);

    // --- toolbar (T) ---

    void draw_toolbar_(imm::Context& ctx, bool mesh_edit);

    // --- navigation gizmo ---

    void draw_nav_gizmo_(imm::Context& ctx);

    // --- N sidebar ---

    void draw_sidebar_(imm::Context& ctx, bool mesh_edit);

    // --- overlay text and object glyphs ---

    std::string view_name_() const;

    /** @brief Viewport text with Blender's soft drop shadow, legible over any render. */
    void shadow_text_(imm::Context& ctx, glm::vec2 p, const std::string& s, const glm::vec4& c) const;

    void draw_overlay_text_(imm::Context& ctx, const ViewProj& vp);

    /** @brief Blender's glyphs for a non-mesh object: camera frustum, light symbols, empty axes. */
    void draw_object_glyph_(imm::Context& ctx, const ViewProj& vp, const Node& node, const glm::mat4& w, const glm::vec4& col);

    /** @brief Object / Edit / Sculpt Mode entries (header dropdown, Ctrl+Tab, viewport RMB in Sculpt). */
    void draw_mode_menu_items_(imm::Context& ctx);

    /**
     * @brief Viewport Shading popover: per shading mode. Solid / Material Preview: ambient
     *        occlusion (the renderer's own AO terms), X-ray. Rendered: the renderer's live
     *        toggles; startup-only features are listed but need a renderer restart
     *        (Properties > Render).
     */
    void draw_shading_popover_(imm::Context& ctx);

    /** @brief Collider Display popover: which colliders the Colliders toggle draws. */
    void draw_colliders_popover_(imm::Context& ctx);

    /** @brief Viewport Overlays popover. */
    void draw_overlays_popover_(imm::Context& ctx);
