#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

void EditorApp::draw_(imm::Context& ctx) {
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

imm::Box EditorApp::area_(imm::Context& ctx, const imm::Box& b) {
    const imm::Box a{b.x + 1, b.y + 1, std::max(0.0f, b.w - 2), std::max(0.0f, b.h - 2)};
    ctx.fill_rounded(a, ctx.style.panel_bg, 6);
    return a;
}

void EditorApp::fill_around_(imm::Context& ctx, const imm::Box& outer, const imm::Box& hole, const glm::vec4& col) {
    ctx.fill({outer.x, outer.y, outer.w, hole.y - outer.y}, col);
    ctx.fill({outer.x, hole.bottom(), outer.w, outer.bottom() - hole.bottom()}, col);
    ctx.fill({outer.x, hole.y, hole.x - outer.x, hole.h}, col);
    ctx.fill({hole.right(), hole.y, outer.right() - hole.right(), hole.h}, col);
}

imm::Box EditorApp::viewport_area_(imm::Context& ctx, const imm::Box& b) {
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

void EditorApp::draw_workspace_(imm::Context& ctx, const imm::Box& c) {
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
    const bool hierarchy = active_type_ == AssetType::Scene || active_type_ == AssetType::Object || active_type_ == AssetType::UI;
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
    if (active_type_ == AssetType::UI) {
        // The UI designer: the asset list above the Widgets palette.
        const imm::Box a = area_(ctx, left);
        const float split = std::max(140.0f, a.h * 0.4f);
        draw_asset_panel_(ctx, {a.x, a.y, a.w, split});
        ctx.fill({a.x + 6, a.y + split, a.w - 12, 1}, ctx.style.border);
        draw_ui_palette_(ctx, {a.x, a.y + split + 2, a.w, a.h - split - 2});
    } else {
        draw_asset_panel_(ctx, area_(ctx, left));
    }
    if (show_bottom_) draw_console_area_(ctx, area_(ctx, console));
    if (hierarchy) draw_outliner_(ctx, area_(ctx, outliner));
    draw_properties_(ctx, area_(ctx, props));
}

bool EditorApp::asset_view_() const {
    return active_type_ == AssetType::Mesh || active_type_ == AssetType::Material || active_type_ == AssetType::Texture ||
           active_type_ == AssetType::Audio;
}

imm::Box EditorApp::area_header_(imm::Context& ctx, const imm::Box& area, float h) {
    const imm::Box bar{area.x, area.y, area.w, h};
    ctx.fill_rounded(bar, ctx.style.header, 6, imm::Context::kTop);
    return bar;
}

imm::Icon EditorApp::icon_for_component_(const std::string& type) {
    using I = imm::Icon;
    if (type == "Transform") return I::Orientation;
    if (type == "MeshRenderer" || type == "SkinnedMeshRenderer") return I::Mesh;
    if (type == "Camera" || type == "CameraController") return I::Camera;
    if (type == "DirectionalLight") return I::Sun;
    if (type == "PointLight") return I::PointLight;
    if (type == "SpotLight") return I::SpotLight;
    if (type == "EnvironmentLight" || type == "ReflectionProbe" || type == "GiProbeVolume") return I::EnvLight;
    if (type == "Rigidbody") return I::Rigidbody;
    if (type.find("Collider") != std::string::npos || type.find("Joint") != std::string::npos || type == "Ragdoll") return I::Collider;
    if (type == "Cloth") return I::Physics;
    if (type == "Terrain") return I::Terrain;
    if (type == "SdfRenderer" || type == "SdfShape") return I::Sphere;
    if (type == "Animator") return I::Play;
    if (type == "Volume") return I::World;
    if (type == "RectTransform") return I::UiAnchor;
    if (type == "Canvas") return I::UiCanvas;
    if (type == "Text" || type == "ThemedText") return I::UiText;
    if (type == "Button" || type == "ThemedButton") return I::UiButton;
    if (type == "Image") return I::Image;
    if (type.find("LayoutGroup") != std::string::npos || type == "LayoutElement" || type == "ContentSizeFitter") return I::UiLayout;
    if (type.find("OnSignal") != std::string::npos) return I::Link;
    if (type == "Theme") return I::Palette;
    if (const ComponentSchema* sc = find_schema(type); sc && sc->category.rfind("UI", 0) == 0) return I::UiWidget;
    return I::Component;
}

std::pair<imm::Icon, glm::vec4> EditorApp::object_icon_(const Node& obj, const imm::Style& st) const {
    using I = imm::Icon;
    const glm::vec4 white = st.text;
    if (!obj.contains("components")) return {I::Empty, white};
    // UI elements: by their most telling component.
    for (const auto& c : obj.at("components").as_seq()) {
        const std::string t = component_type(c);
        if (t == "Canvas") return {I::UiCanvas, et_.outliner.light};
        if (t == "Text" || t == "ThemedText") return {I::UiText, white};
        if (t == "Button" || t == "ThemedButton") return {I::UiButton, et_.outliner.mesh};
        if (t.find("LayoutGroup") != std::string::npos || t == "HudCorner") return {I::UiLayout, white};
        if (t == "Window" || t == "Dialog" || t == "MenuList" || t == "ActionBar" || t == "TabView" || t == "ScrollView" ||
            t == "StatBar" || t == "Hotbar" || t == "ItemGrid" || t == "SettingRow" || t == "MessageLog" || t == "PromptBar" ||
            t == "Collapsible" || t == "ThemedPanel") return {I::UiWidget, et_.outliner.mesh};
        if (t == "Image") return {I::Image, white};
    }
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

} // namespace editor
} // namespace toy
