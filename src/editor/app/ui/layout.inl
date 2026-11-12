// editor/app/ui/layout.inl -- included inside EditorApp's class body.
//
// toyengine's asset editor layout: a top bar (menus, the open asset, play controls); on the
// left the Asset panel (one tab per asset type, then the project's assets of that type --
// clicking one opens it); the viewer in the middle with the Console under it; and on the
// right the Hierarchy (scenes and object assets) above Properties. Every area is a rounded
// panel separated by thin gaps, in the Blender style.

    // =================================================================================
    // UI: top level and workspaces
    // =================================================================================

    void draw_(imm::Context& ctx);

    /** @brief An area: a rounded panel inset by the 1-px gap Blender leaves between areas. */
    imm::Box area_(imm::Context& ctx, const imm::Box& b);

    /** @brief Fills `outer` minus `hole` (the transparent 3D viewport). */
    static void fill_around_(imm::Context& ctx, const imm::Box& outer, const imm::Box& hole, const glm::vec4& col);

    /** @brief The viewport area: only its header is filled; the rest stays see-through. */
    imm::Box viewport_area_(imm::Context& ctx, const imm::Box& b);

    /** @brief Asset panel | viewer (+ console) | hierarchy over properties. */
    void draw_workspace_(imm::Context& ctx, const imm::Box& c);

    /** @brief Is the viewport editing mesh elements (Edit Mode on a mesh asset or a scene object's mesh)? */
    bool mesh_edit_view_() const { return in_edit_mode_(); }

    /** @brief Does the viewport show the private preview scene (mesh / material / texture assets)? */
    bool asset_view_() const;

    /** @brief A Blender area header strip at the top of an area (rounded top corners). */
    imm::Box area_header_(imm::Context& ctx, const imm::Box& area, float h = 26.0f);

    /** @brief The icon a component type shows in the Outliner and Properties. */
    static imm::Icon icon_for_component_(const std::string& type);

    /** @brief The Outliner icon (and tint) for an object: from its most telling component. */
    std::pair<imm::Icon, glm::vec4> object_icon_(const Node& obj, const imm::Style& st) const;
