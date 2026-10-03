// editor/app/ui/layout.inl -- included inside EditorApp's class body.
//
// The window is laid out like Blender's default "Layout" workspace: a top bar (menus,
// workspace tabs, Unity-style play controls), the 3D viewport with an asset browser /
// console below it, and a right column holding the Outliner above the Properties editor.
// Every area is a rounded panel separated by thin gaps, as in Blender 2.8+.

    // =================================================================================
    // UI: top level and workspaces
    // =================================================================================

    void draw_(imm::Context& ctx) {
        const glm::vec2 sz = ctx.canvas_size();
        // No full-window background fill: the 3D render shows through the viewport's rect,
        // so backgrounds are painted per area (and around the viewport) instead.
        const float top_h = 28, status_h = 24;
        draw_topbar_(ctx, {0, 0, sz.x, top_h});
        const imm::Box content{0, top_h, sz.x, std::max(0.0f, sz.y - top_h - status_h)};
        draw_workspace_(ctx, content);
        draw_statusbar_(ctx, {0, sz.y - status_h, sz.x, status_h});
        draw_modals_(ctx);
        handle_shortcuts_(ctx);
    }

    /** @brief An area: a rounded panel inset by the 1-px gap Blender leaves between areas. */
    imm::Box area_(imm::Context& ctx, const imm::Box& b) {
        const imm::Box a{b.x + 1, b.y + 1, std::max(0.0f, b.w - 2), std::max(0.0f, b.h - 2)};
        ctx.fill_rounded(a, ctx.style.panel_bg, 6);
        return a;
    }

    /** @brief Fills `outer` minus `hole` (the transparent 3D viewport). */
    static void fill_around_(imm::Context& ctx, const imm::Box& outer, const imm::Box& hole, const glm::vec4& col) {
        ctx.fill({outer.x, outer.y, outer.w, hole.y - outer.y}, col);
        ctx.fill({outer.x, hole.bottom(), outer.w, outer.bottom() - hole.bottom()}, col);
        ctx.fill({outer.x, hole.y, hole.x - outer.x, hole.h}, col);
        ctx.fill({hole.right(), hole.y, outer.right() - hole.right(), hole.h}, col);
    }

    /** @brief The viewport area: only its header is filled; the rest stays see-through. */
    imm::Box viewport_area_(imm::Context& ctx, const imm::Box& b) {
        const imm::Box a{b.x + 1, b.y + 1, std::max(0.0f, b.w - 2), std::max(0.0f, b.h - 2)};
        // Gaps around the area, then rounded-corner masks so the render reads as a rounded area.
        fill_around_(ctx, b, a, ctx.style.window_bg);
        const float r = 6;
        const glm::vec4 bg = ctx.style.window_bg;
        auto corner = [&](glm::vec2 c, float a0) {
            for (int i = 0; i < 6; ++i) {
                const float u0 = a0 + 1.5707963f * i / 6, u1 = a0 + 1.5707963f * (i + 1) / 6;
                const glm::vec2 p0 = c + glm::vec2(std::cos(u0), std::sin(u0)) * r, p1 = c + glm::vec2(std::cos(u1), std::sin(u1)) * r;
                const glm::vec2 q0 = c + glm::vec2(std::cos(u0) >= 0 ? r : -r, std::sin(u0) >= 0 ? r : -r) * 1.0f;
                ctx.triangle(p0, p1, glm::vec2(std::abs(std::cos(u0)) > std::abs(std::sin(u0)) ? q0.x : (std::cos(u0) >= 0 ? c.x + r : c.x - r),
                                               std::abs(std::cos(u0)) > std::abs(std::sin(u0)) ? (std::sin(u0) >= 0 ? c.y + r : c.y - r) : q0.y), bg);
            }
        };
        corner({a.x + r, a.bottom() - r}, 1.5707963f);
        corner({a.right() - r, a.bottom() - r}, 0.0f);
        return a;
    }

    /** @brief Viewport + bottom area on the left, Outliner over Properties on the right. */
    void draw_workspace_(imm::Context& ctx, const imm::Box& c) {
        const bool mesh_edit = mesh_edit_view_();
        if (maximized_) { draw_viewport_(ctx, viewport_area_(ctx, c), mesh_edit); return; }
        const float gap = 4;
        right_w_ = std::clamp(right_w_, 260.0f, std::max(260.0f, c.w * 0.5f));
        bottom_h_ = std::clamp(bottom_h_, 70.0f, std::max(70.0f, c.h * 0.6f));
        outliner_h_ = std::clamp(outliner_h_, 90.0f, std::max(90.0f, c.h - 160.0f));
        const float left_w = c.w - right_w_;
        const imm::Box viewport{c.x, c.y, left_w, show_bottom_ ? c.h - bottom_h_ : c.h};
        const imm::Box bottom{c.x, viewport.bottom(), left_w, bottom_h_};
        const imm::Box outliner{c.x + left_w, c.y, right_w_, outliner_h_};
        const imm::Box props{c.x + left_w, c.y + outliner_h_, right_w_, c.h - outliner_h_};

        float sx = left_w;
        if (ctx.splitter("ws_split_right", {c.x + left_w - gap * 0.5f, c.y, gap, c.h}, true, &sx, c.w * 0.5f, c.w - 260.0f).changed) {
            right_w_ = c.w - sx;
        }
        if (show_bottom_) {
            float by = viewport.h;
            if (ctx.splitter("ws_split_bottom", {c.x, viewport.bottom() - gap * 0.5f, left_w, gap}, false, &by, c.h * 0.4f, c.h - 70.0f).changed) {
                bottom_h_ = c.h - by;
            }
        }
        float oy = outliner_h_;
        if (ctx.splitter("ws_split_outliner", {outliner.x, outliner.bottom() - gap * 0.5f, right_w_, gap}, false, &oy, 90.0f, c.h - 160.0f).changed) {
            outliner_h_ = oy;
        }

        // Non-viewport areas paint their own gap background first.
        for (const imm::Box& b : {bottom, outliner, props}) ctx.fill(b, ctx.style.window_bg);
        draw_viewport_(ctx, viewport_area_(ctx, viewport), mesh_edit);
        if (show_bottom_) draw_bottom_area_(ctx, area_(ctx, bottom));
        draw_outliner_(ctx, area_(ctx, outliner));
        draw_properties_(ctx, area_(ctx, props));
    }

    /** @brief Is the viewport editing mesh elements (scene edit mode, or a standalone mesh)? */
    bool mesh_edit_view_() const {
        if (tab_ == Tab::Modeling && standalone_mesh_ && asset_kind_ == AssetKind::Mesh) return true;
        return in_edit_mode_();
    }

    /** @brief Does the viewport show the private preview scene instead of the edited scene? */
    bool asset_view_() const {
        return (tab_ == Tab::Shading && asset_kind_ == AssetKind::Material) ||
               (tab_ == Tab::Modeling && standalone_mesh_ && asset_kind_ == AssetKind::Mesh);
    }

    /** @brief A Blender area header strip at the top of an area (rounded top corners). */
    imm::Box area_header_(imm::Context& ctx, const imm::Box& area, float h = 26.0f) {
        const imm::Box bar{area.x, area.y, area.w, h};
        ctx.fill_rounded(bar, ctx.style.header, 6, imm::Context::kTop);
        return bar;
    }

    /** @brief The icon a component type shows in the Outliner and Properties. */
    static imm::Icon icon_for_component_(const std::string& type) {
        using I = imm::Icon;
        if (type == "Transform") return I::Orientation;
        if (type == "MeshRenderer" || type == "SkinnedMeshRenderer") return I::Mesh;
        if (type == "Camera" || type == "CameraController") return I::Camera;
        if (type == "DirectionalLight") return I::Sun;
        if (type == "PointLight") return I::PointLight;
        if (type == "SpotLight") return I::SpotLight;
        if (type == "EnvironmentLight" || type == "ReflectionProbe" || type == "GiProbeVolume") return I::EnvLight;
        if (type == "Rigidbody") return I::Rigidbody;
        if (type.find("Collider") != std::string::npos || type == "HingeJoint") return I::Collider;
        if (type == "Cloth") return I::Physics;
        if (type == "Terrain") return I::Terrain;
        if (type == "SdfRenderer" || type == "SdfShape") return I::Sphere;
        if (type == "Animator") return I::Play;
        if (type == "Volume") return I::World;
        return I::Component;
    }

    /** @brief The Outliner icon (and tint) for an object: from its most telling component. */
    std::pair<imm::Icon, glm::vec4> object_icon_(const Node& obj, const imm::Style& st) const {
        using I = imm::Icon;
        const glm::vec4 white = st.text;
        if (!obj.contains("components")) return {I::Empty, white};
        for (const auto& c : obj.at("components").as_seq()) {
            const std::string t = component_type(c);
            if (t == "Camera") return {I::Camera, et_.outliner.camera};
            if (t.find("Light") != std::string::npos) return {icon_for_component_(t), et_.outliner.light};
        }
        for (const auto& c : obj.at("components").as_seq()) {
            const std::string t = component_type(c);
            if (t == "MeshRenderer" || t == "SkinnedMeshRenderer") return {I::Mesh, et_.outliner.mesh};
            if (t == "Terrain") return {I::Terrain, et_.outliner.terrain};
            if (t == "SdfRenderer") return {I::Sphere, et_.outliner.mesh};
            if (t == "ReflectionProbe" || t == "EnvironmentLight") return {I::EnvLight, et_.outliner.light};
        }
        return {I::Empty, white};
    }
