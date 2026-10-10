// editor/app/ui/outliner.inl -- included inside EditorApp's class body.
//
// The Hierarchy: exactly what the open file holds. For a scene, the root row is the
// file's `scene:` block (named by scene_name) and its rows are root_objects in file order,
// nested children, then every component in order (Transform included). For an object asset
// the root row is the object itself. Prefab instances (`prefab:`) show a link icon; the
// children they inherit from the object asset are rows like any other (ui/instances.inl),
// dimmed until they override something, and an accent bar marks a row with overrides. Rows
// have eye (viewport visibility) and monitor (enabled in the game) toggles.

    void draw_outliner_(imm::Context& ctx, const imm::Box& area);

    void draw_outliner_list_(imm::Context& ctx, const Node& list);

    /**
     * @brief True for a row that is off screen and collapsed: the caller reserves its height
     *        instead of drawing it. A 1000-object scene then pays for the rows in view, not for
     *        every row's widgets, icons and strings each frame -- while scrolling, the content
     *        height and drop targets stay exactly as if every row were drawn. An open row is
     *        always drawn (its children's height isn't known without walking them), and so is
     *        the row being renamed.
     */
    bool outliner_row_skippable_(imm::Context& ctx, ObjectId id, bool flat);

    void draw_outliner_row_(imm::Context& ctx, ObjectId id, bool flat);

    void draw_object_context_menu_(imm::Context& ctx, ObjectId target);
