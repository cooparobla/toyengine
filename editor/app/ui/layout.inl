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

    /** @brief Asset panel | viewer (+ console) | hierarchy over properties. */
    void draw_workspace_(imm::Context& ctx, const imm::Box& c) {
        const bool mesh_edit = mesh_edit_view_();
        if (maximized_) { draw_viewport_(ctx, viewport_area_(ctx, c), mesh_edit); return; }
        const float gap = 4;
        left_w_ = std::clamp(left_w_, 200.0f, std::max(200.0f, c.w * 0.35f));
        right_w_ = std::clamp(right_w_, 260.0f, std::max(260.0f, c.w * 0.4f));
        bottom_h_ = std::clamp(bottom_h_, 60.0f, std::max(60.0f, c.h * 0.5f));
        outliner_h_ = std::clamp(outliner_h_, 90.0f, std::max(90.0f, c.h - 160.0f));
        const float mid_w = std::max(100.0f, c.w - left_w_ - right_w_);
        const imm::Box left{c.x, c.y, left_w_, c.h};
        const imm::Box viewport{c.x + left_w_, c.y, mid_w, show_bottom_ ? c.h - bottom_h_ : c.h};
        const imm::Box console{c.x + left_w_, viewport.bottom(), mid_w, bottom_h_};
        const bool hierarchy = active_type_ == AssetType::Scene || active_type_ == AssetType::Object;
        const imm::Box outliner{c.x + left_w_ + mid_w, c.y, right_w_, hierarchy ? outliner_h_ : 0.0f};
        const imm::Box props{c.x + left_w_ + mid_w, c.y + outliner.h, right_w_, c.h - outliner.h};

        float lx = left_w_;
        if (ctx.splitter("ws_split_left", {c.x + left_w_ - gap * 0.5f, c.y, gap, c.h}, true, &lx, 200.0f, c.w * 0.35f).changed) left_w_ = lx;
        float sx = c.w - right_w_;
        if (ctx.splitter("ws_split_right", {c.x + c.w - right_w_ - gap * 0.5f, c.y, gap, c.h}, true, &sx, c.w * 0.6f, c.w - 260.0f).changed) {
            right_w_ = c.w - sx;
        }
        if (show_bottom_) {
            float by = viewport.h;
            if (ctx.splitter("ws_split_bottom", {viewport.x, viewport.bottom() - gap * 0.5f, mid_w, gap}, false, &by, c.h * 0.5f, c.h - 60.0f).changed) {
                bottom_h_ = c.h - by;
            }
        }
        if (hierarchy) {
            float oy = outliner_h_;
            if (ctx.splitter("ws_split_outliner", {outliner.x, outliner.bottom() - gap * 0.5f, right_w_, gap}, false, &oy, 90.0f, c.h - 160.0f).changed) {
                outliner_h_ = oy;
            }
        }

        // Non-viewport areas paint their own gap background first.
        ctx.fill(left, ctx.style.window_bg);
        if (show_bottom_) ctx.fill(console, ctx.style.window_bg);
        if (hierarchy) ctx.fill(outliner, ctx.style.window_bg);
        ctx.fill(props, ctx.style.window_bg);
        draw_viewport_(ctx, viewport_area_(ctx, viewport), mesh_edit);
        draw_asset_panel_(ctx, area_(ctx, left));
        if (show_bottom_) draw_console_area_(ctx, area_(ctx, console));
        if (hierarchy) draw_outliner_(ctx, area_(ctx, outliner));
        draw_properties_(ctx, area_(ctx, props));
    }

    /** @brief Is the viewport editing mesh elements (Edit Mode on a mesh asset or a scene object's mesh)? */
    bool mesh_edit_view_() const { return in_edit_mode_(); }

    /** @brief Does the viewport show the private preview scene (mesh / material / texture assets)? */
    bool asset_view_() const {
        return active_type_ == AssetType::Mesh || active_type_ == AssetType::Material || active_type_ == AssetType::Texture;
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
