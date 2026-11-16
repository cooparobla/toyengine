#include "editor/app/editor_app.h"
#include "editor/app/ui/topbar.h"
#include "editor/app/ui/node_graph.h"
#include "editor/app/ui/ui_canvas.h"
#include "editor/app/ui/properties.h"
#include "editor/app/ui/assets.h"
#include "editor/app/ui/statusbar.h"
#include "editor/app/ui/outliner.h"
#include "editor/app/thumbnail_cache.h"
#include "editor/app/ui/metrics.h"

#include <algorithm>
#include <cctype>

namespace toy {
namespace editor {

void EditorApp::draw_(imm::Context& ctx) {
    if (thumbnails_) thumbnails_->begin_frame();   // a fresh per-frame load budget
    const glm::vec2 sz = ctx.canvas_size();
    // No full-window background fill: the 3D render shows through the viewport's rect,
    // so backgrounds are painted per area (and around the viewport) instead.
    const float top_h = ui::k_topbar_h, status_h = ui::k_statusbar_h;
    topbar_panel_->draw_topbar_(ctx, {0, 0, sz.x, top_h});
    const imm::Box content{0, top_h, sz.x, std::max(0.0f, sz.y - top_h - status_h)};
    draw_workspace_(ctx, content);
    statusbar_panel_->draw_statusbar_(ctx, {0, sz.y - status_h, sz.x, status_h});
    draw_modals_(ctx);
    handle_shortcuts_(ctx);
}

imm::Box EditorApp::area_(imm::Context& ctx, const imm::Box& b) {
    const imm::Box a{b.x + 1, b.y + 1, std::max(0.0f, b.w - 2), std::max(0.0f, b.h - 2)};
    ctx.fill_rounded(a, ctx.style.panel_bg, ui::k_area_radius);
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
    const float r = ui::k_area_radius;
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
    const float gap = ui::k_area_gap;
    left_w_   = ui::clamp_split(left_w_,   ui::k_min_left_w,   c.w, ui::k_max_left_frac);
    right_w_  = ui::clamp_split(right_w_,  ui::k_min_right_w,  c.w, ui::k_max_right_frac);
    bottom_h_ = ui::clamp_split(bottom_h_, ui::k_min_bottom_h, c.h, ui::k_max_bottom_frac);
    outliner_h_ = std::clamp(outliner_h_, ui::k_min_outliner_h,
                             std::max(ui::k_min_outliner_h, c.h - ui::k_outliner_slack));
    const float mid_w = std::max(100.0f, c.w - left_w_ - right_w_);
    const imm::Box left{c.x, c.y, left_w_, c.h};
    const imm::Box viewport{c.x + left_w_, c.y, mid_w, show_bottom_ ? c.h - bottom_h_ : c.h};
    const imm::Box console{c.x + left_w_, viewport.bottom(), mid_w, bottom_h_};
    const bool hierarchy = active_type_ == AssetType::Scene || active_type_ == AssetType::Object || active_type_ == AssetType::UI;
    const imm::Box outliner{c.x + left_w_ + mid_w, c.y, right_w_, hierarchy ? outliner_h_ : 0.0f};
    const imm::Box props{c.x + left_w_ + mid_w, c.y + outliner.h, right_w_, c.h - outliner.h};

    float lx = left_w_;
    if (ctx.splitter("ws_split_left", {c.x + left_w_ - gap * 0.5f, c.y, gap, c.h}, true, &lx,
                     ui::k_min_left_w, c.w * ui::k_max_left_frac).changed) left_w_ = lx;
    float sx = c.w - right_w_;
    // Stated as the splitter's own x, so this reads as the mirror of the left one's limits.
    if (ctx.splitter("ws_split_right", {c.x + c.w - right_w_ - gap * 0.5f, c.y, gap, c.h}, true, &sx,
                     c.w * (1.0f - ui::k_max_right_frac), c.w - ui::k_min_right_w).changed) {
        right_w_ = c.w - sx;
    }
    if (show_bottom_) {
        float by = viewport.h;
        if (ctx.splitter("ws_split_bottom", {viewport.x, viewport.bottom() - gap * 0.5f, mid_w, gap}, false, &by,
                         c.h * (1.0f - ui::k_max_bottom_frac), c.h - ui::k_min_bottom_h).changed) {
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
    if (active_type_ == AssetType::NodeGraph) {
        // The asset list above the graph's own node list, mirroring the UI designer's split.
        const imm::Box a = area_(ctx, left);
        const float split = std::max(ui::k_left_split_min, a.h * ui::k_left_split_frac);
        assets_panel_->draw_asset_panel_(ctx, {a.x, a.y, a.w, split});
        ctx.fill({a.x + 6, a.y + split, a.w - 12, 1}, ctx.style.border);
        node_graph_panel_->draw_graph_palette_(ctx, {a.x, a.y + split + 2, a.w, a.h - split - 2});
    } else if (active_type_ == AssetType::UI) {
        // The UI designer: the asset list above the Widgets palette.
        const imm::Box a = area_(ctx, left);
        const float split = std::max(ui::k_left_split_min, a.h * ui::k_left_split_frac);
        assets_panel_->draw_asset_panel_(ctx, {a.x, a.y, a.w, split});
        ctx.fill({a.x + 6, a.y + split, a.w - 12, 1}, ctx.style.border);
        ui_canvas_panel_->draw_ui_palette_(ctx, {a.x, a.y + split + 2, a.w, a.h - split - 2});
    } else {
        assets_panel_->draw_asset_panel_(ctx, area_(ctx, left));
    }
    if (show_bottom_) assets_panel_->draw_console_area_(ctx, area_(ctx, console));
    if (hierarchy) outliner_panel_->draw_outliner_(ctx, area_(ctx, outliner));
    properties_panel_->draw_properties_(ctx, area_(ctx, props));
}

bool EditorApp::asset_view_() const {
    return active_type_ == AssetType::Mesh || active_type_ == AssetType::Material || active_type_ == AssetType::Texture ||
           active_type_ == AssetType::Audio;
}

imm::Box EditorApp::area_header_(imm::Context& ctx, const imm::Box& area, float h) {
    const imm::Box bar{area.x, area.y, area.w, h};
    ctx.fill_rounded(bar, ctx.style.header, ui::k_area_radius, imm::Context::kTop);
    return bar;
}

std::string EditorApp::search_box_in_(imm::Context& ctx, const char* id, const imm::Box& box,
                                      std::string& filter, const char* placeholder) {
    ctx.input_text_box(id, box, &filter, "");
    const std::string live = ctx.editing_text(id).value_or(filter);
    if (live.empty()) {
        // The magnifier and the prompt are drawn only while the field is empty, so neither
        // has to be budgeted for in the space the typed text gets.
        const float gw = 14.0f;
        ctx.icon(imm::Icon::Search, {box.x + 4, box.y + (box.h - gw) * 0.5f, gw, gw}, ctx.style.text_disabled);
        ctx.text_in({box.x + gw + 7, box.y, std::max(0.0f, box.w - gw - 7), box.h}, placeholder,
                    ctx.style.text_disabled, 0.0f);
    }
    test_rects_[id] = box;
    std::string needle = live;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return needle;
}

std::string EditorApp::search_box_(imm::Context& ctx, const char* id, std::string& filter,
                                   const char* placeholder) {
    return search_box_in_(ctx, id, ctx.next_box(22), filter, placeholder);
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
    if (type == "Terrain" || type == "TileTerrain" || type == "HeightmapTerrain") return I::Terrain;
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
        if (t == "Terrain" || t == "TileTerrain" || t == "HeightmapTerrain") return {I::Terrain, et_.outliner.terrain};
        if (t == "SdfRenderer") return {I::Sphere, et_.outliner.mesh};
        if (t == "ReflectionProbe" || t == "EnvironmentLight") return {I::EnvLight, et_.outliner.light};
    }
    return {I::Empty, white};
}

} // namespace editor
} // namespace toy
