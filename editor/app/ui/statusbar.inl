// editor/app/ui/statusbar.inl -- included inside EditorApp's class body.
//
// Blender's status bar: mouse-button hints for the current context at the left, the
// latest report in the middle, scene statistics and the version at the right.

    void draw_statusbar_(imm::Context& ctx, const imm::Box& b);

    /** @brief "Scene | Objects 1/4 | Verts 8 | Faces 6 | Tris 12" (edit mode: selected/total). */
    std::string scene_stats_();
