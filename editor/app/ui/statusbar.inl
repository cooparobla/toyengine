// editor/app/ui/statusbar.inl -- included inside EditorApp's class body.
//
// Blender's status bar: mouse-button hints for the current context at the left, the
// latest report in the middle, scene statistics and the version at the right.

    void draw_statusbar_(imm::Context& ctx, const imm::Box& b) {
        using I = imm::Icon;
        ctx.fill(b, glm::vec4(0.137f, 0.137f, 0.137f, 1.0f));
        // --- hints ---
        std::vector<std::pair<I, std::string>> hints;
        if (modal_.active()) {
            hints = {{I::MouseLeft, "Confirm"}, {I::MouseRight, "Cancel"}, {I::Keyboard, "X Y Z  Axis"}, {I::Keyboard, "Ctrl  Snap"},
                     {I::Keyboard, "Shift  Precision"}};
        } else if (nav_active_) {
            hints = {{I::MouseMiddle, "Orbit"}, {I::Keyboard, "Shift  Pan"}, {I::Keyboard, "Ctrl  Zoom"}};
        } else if (mesh_edit_view_()) {
            hints = {{I::MouseLeft, "Select"}, {I::MouseMiddle, "Rotate View"}, {I::MouseRight, "Context Menu"},
                     {I::Keyboard, "G R S  Transform"}, {I::Keyboard, "E  Extrude"}, {I::Keyboard, "Tab  Object Mode"}};
        } else {
            hints = {{I::MouseLeft, "Select"}, {I::MouseMiddle, "Rotate View"}, {I::MouseRight, "Object Context Menu"},
                     {I::Keyboard, "Shift A  Add"}, {I::Keyboard, "Tab  Edit Mode"}};
        }
        float x = b.x + 8;
        for (const auto& [ic, text] : hints) {
            ctx.icon(ic, {x, b.y + 4, b.h - 8, b.h - 8}, ctx.style.text_dim);
            x += b.h - 4;
            const float tw = ctx.text_width(text);
            ctx.text_in({x, b.y, tw + 4, b.h}, text, ctx.style.text_dim, 0.0f);
            x += tw + 16;
        }
        // --- latest report ---
        const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - status_time_).count();
        if (!status_.empty() && age < 8.0) {
            const I ic = status_level_ == 2 ? I::Error : status_level_ == 1 ? I::Warning : I::Info;
            const glm::vec4 c = status_level_ == 2 ? ctx.style.error : status_level_ == 1 ? ctx.style.warning : ctx.style.text;
            ctx.icon(ic, {x + 10, b.y + 4, b.h - 8, b.h - 8}, status_level_ == 0 ? ctx.style.text_dim : glm::vec4(1));
            ctx.text_in({x + 10 + b.h, b.y, b.w * 0.4f, b.h}, status_, c, 0.0f);
        }
        // --- stats, Blender style ---
        std::string stats = scene_stats_();
        if (playing()) stats = "PLAYING  |  " + stats;
        stats += "  |  toyengine 0.1";
        const float sw = ctx.text_width(stats);
        ctx.text_in({b.right() - sw - 10, b.y, sw + 6, b.h}, stats, ctx.style.text_dim, 0.0f);
    }

    /** @brief "Scene | Objects 1/4 | Verts 8 | Faces 6 | Tris 12" (edit mode: selected/total). */
    std::string scene_stats_() {
        char buf[200];
        if (mesh_edit_view_() && mesh_.open()) {
            const auto& m = mesh_.mesh;
            const auto vs = mesh_.selection.affected_vertices(m);
            const auto fs_ = mesh_.selection.affected_faces(m);
            std::snprintf(buf, sizeof(buf), "%s  |  Verts %zu/%zu  |  Faces %zu/%zu  |  Tris %zu", mesh_.name.c_str(), vs.size(),
                          m.positions.size(), fs_.size(), m.faces.size(), m.triangle_count());
            return buf;
        }
        size_t verts = 0, faces = 0, tris = 0, objects = 0;
        for (const auto& [id, live] : sync_.live_objects()) {
            ++objects;
            const Node* n = doc_.find(id);
            if (!n || !live || !live->active()) continue;
            if (const CachedMesh* cm = mesh_for_object_(*n)) {
                verts += cm->mesh.positions.size();
                faces += cm->mesh.faces.size();
                tris += cm->mesh.triangle_count();
            }
        }
        std::snprintf(buf, sizeof(buf), "%s  |  Objects %zu/%zu  |  Verts %zu  |  Faces %zu  |  Tris %zu", doc_.scene_name().c_str(),
                      doc_.selection().size(), objects, verts, faces, tris);
        return buf;
    }
