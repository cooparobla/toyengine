#include "editor/app/ui/paint.h"
#include "editor/app/ui/viewport_chrome.h"
#include "editor/app/ui/sculpt.h"
#include "editor/app/ui/tile_mode.h"
#include "editor/app/ui/terrain.h"

#include "editor/app/editor_app.h"
#include "editor/app/ui/instance_paint.h"
#include "editor/app/ui/weights.h"

namespace toy {
namespace editor {

bool PaintPanel::in_paint_mode_() const {
    return app_.mesh_mode_target_() && is_paint_mode_(app_.mode_);
}

bool PaintPanel::in_weight_paint_() const {
    return app_.mesh_mode_target_() && app_.mode_ == InteractionMode::WeightPaint;
}

bool PaintPanel::in_brush_mode_() const {
    return app_.in_sculpt_mode_() || in_paint_mode_() || app_.terrain_panel_->in_terrain_mode_() ||
           app_.tile_mode_panel_->in_tile_mode_() || app_.instance_paint_panel_->in_instance_mode_();
}

PaintSettings& PaintPanel::paint_settings_() {
    return app_.mode_ == InteractionMode::WeightPaint ? app_.wpaint_ : app_.vpaint_;
}

PaintPreview::Show PaintPanel::paint_show_() const {
    return {app_.mode_ == InteractionMode::WeightPaint, app_.active_group_};
}

PaintSettings& PaintPanel::vertex_paint_settings() {
    return app_.vpaint_;
}

PaintSettings& PaintPanel::weight_paint_settings() {
    return app_.wpaint_;
}

uint32_t PaintPanel::active_vertex_group() const {
    return app_.active_group_;
}

void PaintPanel::set_active_vertex_group(uint32_t g) {
    app_.active_group_ = g;
}

bool PaintPanel::paint_stroking() const {
    return app_.paint_stroke_.active;
}

void PaintPanel::mode_begin_(InteractionMode m) {
    if (m == InteractionMode::Sculpt) app_.sculpt_panel_->sculpt_begin_();
    else if (is_paint_mode_(m)) paint_begin_();
}

void PaintPanel::mode_end_(InteractionMode m) {
    if (m == InteractionMode::Sculpt) app_.sculpt_panel_->sculpt_end_();
    else if (is_paint_mode_(m)) paint_end_();
}

void PaintPanel::paint_begin_() {
    if (app_.mesh_.mesh.faces.empty()) return;
    if (app_.active_group_ >= app_.mesh_.mesh.groups.size()) app_.active_group_ = 0;
    try {
        app_.sculpt_cache_.build(app_.mesh_.mesh);
        app_.paint_preview_.build(app_.engine_.device(), app_.engine_.allocator(), app_.mesh_.mesh, app_.sculpt_cache_, paint_show_());
    } catch (const std::exception& e) {
        app_.log_error(std::string(app_.mode_ == InteractionMode::WeightPaint ? "Weight Paint: " : "Vertex Paint: ") + e.what());
        app_.paint_active_ = false;
        return;
    }
    app_.paint_active_ = true;
    app_.paint_geom_ = app_.mesh_.geometry_revision;
    app_.paint_pos_ = app_.mesh_.position_revision;
    app_.paint_shown_ = paint_show_();
    assign_paint_preview_();
    app_.apply_shading_();
    app_.tool_ = EditorApp::Tool::Select;
}

void PaintPanel::paint_end_() {
    if (app_.paint_stroke_.active) end_paint_stroke_();
    app_.paint_resize_.active = false;
    if (!app_.paint_active_) return;
    app_.paint_active_ = false;
    restore_paint_materials_();
    app_.paint_preview_.reset();
    app_.scene_uploaded_revision_ = 0;   // push_mesh_to_scene_ re-exports the painted mesh once
    app_.uploaded_revision_ = 0;         // and the mesh viewer re-uploads it (with its own material)
    app_.apply_shading_();
}

std::vector<std::pair<ObjectId, coopa::gfx::engine::components::MeshRenderer*>> PaintPanel::edited_mesh_renderers_() {
    std::vector<std::pair<ObjectId, coopa::gfx::engine::components::MeshRenderer*>> out;
    if (app_.active_type_ == AssetType::Mesh) {
        if (auto* mr = app_.preview_renderer_()) out.push_back({0, mr});
        return out;
    }
    const fs::path dir = (app_.doc_.path().empty() ? app_.sync_.fallback_path : app_.doc_.path()).parent_path();
    for (const auto& [id, live] : app_.sync_.live_objects()) {
        const int ci = app_.doc_.find_component(id, "MeshRenderer");
        if (ci < 0 || !live) continue;
        const std::string key = app_.object_mesh_key_(app_.instances_panel_->effective_(*app_.doc_.find(id)));   // a skinned mesh's too
        const fs::path p = coopa::yaml::resolve_variant(app_.engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
        if (p != app_.mesh_.path) continue;
        if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) out.push_back({id, mr});
    }
    return out;
}

void PaintPanel::assign_paint_preview_() {
    if (!app_.paint_preview_.mesh()) return;
    auto handle = app_.engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/paint_mesh", app_.paint_preview_.mesh());
    for (auto& [id, mr] : edited_mesh_renderers_()) {
        if (!app_.paint_saved_shader_.count(id)) app_.paint_saved_shader_[id] = mr->material.shader;
        mr->material.shader = "editor_paint";
        mr->set_mesh(handle);
    }
    app_.mesh_cache_.clear();
    app_.scene_uploaded_revision_ = app_.mesh_.geometry_revision;
}

void PaintPanel::restore_paint_materials_() {
    for (auto& [id, mr] : edited_mesh_renderers_()) {
        auto it = app_.paint_saved_shader_.find(id);
        if (it != app_.paint_saved_shader_.end() && mr->material.shader == "editor_paint") mr->material.shader = it->second;
    }
    app_.paint_saved_shader_.clear();
}

void PaintPanel::paint_frame_() {
    if (!app_.paint_active_) return;
    if (!app_.mesh_.live() && app_.paint_geom_ != app_.mesh_.geometry_revision) {
        const bool same_topology = SculptCache::topology_key(app_.mesh_.mesh) == app_.sculpt_cache_.topo_key;
        if (app_.active_group_ >= app_.mesh_.mesh.groups.size()) app_.active_group_ = app_.mesh_.mesh.groups.empty() ? 0u : static_cast<uint32_t>(app_.mesh_.mesh.groups.size() - 1);
        try {
            app_.sculpt_cache_.build(app_.mesh_.mesh);
            if (!same_topology) {
                app_.paint_preview_.build(app_.engine_.device(), app_.engine_.allocator(), app_.mesh_.mesh, app_.sculpt_cache_, paint_show_());
                assign_paint_preview_();
            }
        } catch (const std::exception& e) {
            app_.log_error(std::string("Paint: ") + e.what());
        }
        app_.paint_geom_ = app_.mesh_.geometry_revision;
        app_.paint_pos_ = 0;
    }
    app_.scene_uploaded_revision_ = app_.mesh_.geometry_revision;   // the preview shows it; no re-export
    const PaintPreview::Show show = paint_show_();
    if (app_.paint_pos_ != app_.mesh_.position_revision || show.weights != app_.paint_shown_.weights || show.group != app_.paint_shown_.group) {
        app_.paint_preview_.update(app_.mesh_.mesh, app_.sculpt_cache_, show, app_.engine_.frame_slot());
        app_.paint_pos_ = app_.mesh_.position_revision;
        app_.paint_shown_ = show;
    }
}

float PaintPanel::brush_local_radius_(const ViewProj& vp, const glm::vec3& local_hit, float radius_px) {
    const glm::mat4 w = app_.mesh_world_();
    const glm::vec3 world = glm::vec3(w * glm::vec4(local_hit, 1.0f));
    const float scale = std::max({glm::length(glm::vec3(w[0])), glm::length(glm::vec3(w[1])), glm::length(glm::vec3(w[2])), 1e-6f});
    return radius_px * vp.world_per_pixel(world) / scale;
}

uint32_t PaintPanel::paint_group_(EditMesh& m) {
    if (m.groups.empty()) {
        app_.active_group_ = m.group_index("Group");
        app_.log_info("Weight Paint: added vertex group \"Group\" to paint into");
    }
    return std::min<uint32_t>(app_.active_group_, static_cast<uint32_t>(m.groups.size() - 1));
}

void PaintPanel::paint_dab_at_(const ViewProj& vp, glm::vec2 px, std::vector<uint32_t>& changed) {
    glm::vec3 hit, n;
    if (!app_.sculpt_panel_->sculpt_hit_(vp, px, hit, n)) return;
    if (std::abs(app_.sculpt_grid_.cell() - app_.paint_stroke_.radius) > app_.paint_stroke_.radius * 0.5f) app_.sculpt_grid_.build(app_.mesh_.mesh, app_.paint_stroke_.radius);
    PaintSettings s = paint_settings_();
    s.tool = app_.paint_stroke_.tool;
    PaintDab d;
    d.center = hit;
    d.view_dir = -n;
    d.radius = app_.paint_stroke_.radius;
    d.strength = s.strength;
    d.invert = app_.paint_stroke_.invert;
    SculptDab sd;   // symmetry is shared with Sculpt: mirror the dab's centre and view
    sd.center = d.center;
    sd.view_dir = d.view_dir;
    sd.radius = d.radius;
    const bool weights = app_.mode_ == InteractionMode::WeightPaint;
    const uint32_t group = weights ? paint_group_(app_.mesh_.mesh) : 0u;
    for_each_symmetric(sd, mirror_images(s.symmetry, app_.mesh_world_()), [&](const SculptDab& md) {
        PaintDab pd = d;
        pd.center = md.center;
        pd.view_dir = md.view_dir;
        if (weights) weight_paint_dab(app_.mesh_.mesh, app_.sculpt_cache_, app_.sculpt_grid_, s, group, pd, changed);
        else vertex_paint_dab(app_.mesh_.mesh, app_.sculpt_cache_, app_.sculpt_grid_, s, pd, changed);
    });
}

void PaintPanel::begin_paint_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl) {
    glm::vec3 hit, n;
    if (!app_.sculpt_panel_->sculpt_hit_(vp, px, hit, n)) return;
    const PaintSettings& s = paint_settings_();
    app_.paint_stroke_ = PaintStroke{};
    app_.paint_stroke_.active = true;
    app_.paint_stroke_.tool = shift ? PaintTool::Blur : s.tool;
    app_.paint_stroke_.invert = ctrl;
    app_.paint_stroke_.radius = std::max(1e-5f, brush_local_radius_(vp, hit, s.radius_px));
    app_.paint_stroke_.last_px = px;
    app_.mesh_.begin_live();
    app_.sculpt_grid_.build(app_.mesh_.mesh, app_.paint_stroke_.radius);
    std::vector<uint32_t> changed;
    paint_dab_at_(vp, px, changed);
    if (!changed.empty()) app_.mesh_.touch_live();
}

void PaintPanel::continue_paint_stroke_(const ViewProj& vp, glm::vec2 px) {
    const PaintSettings& s = paint_settings_();
    const float step = std::max(1.0f, s.spacing * s.radius_px);
    glm::vec2 cur = app_.paint_stroke_.last_px;
    float dist = glm::distance(px, cur);
    std::vector<uint32_t> changed;
    int guard = 0;
    while (dist >= step && guard++ < 256) {
        cur += (px - cur) / dist * step;
        paint_dab_at_(vp, cur, changed);
        dist = glm::distance(px, cur);
    }
    app_.paint_stroke_.last_px = cur;
    if (!changed.empty()) app_.mesh_.touch_live();
}

void PaintPanel::end_paint_stroke_() {
    if (!app_.paint_stroke_.active) return;
    app_.paint_stroke_.active = false;
    const bool weights = app_.mode_ == InteractionMode::WeightPaint;
    app_.mesh_.end_live(std::string(weights ? "Weight Paint " : "Vertex Paint ") + paint_tool_name(app_.paint_stroke_.tool));
    app_.paint_geom_ = app_.mesh_.geometry_revision;   // no topology change: the cache is still right
}

void PaintPanel::paint_sample_(const ViewProj& vp, glm::vec2 px) {
    glm::vec3 o, d;
    app_.mesh_tools_panel_->mesh_local_ray_(vp, px, o, d);
    auto h = app_.sculpt_cache_.bvh.raycast(app_.mesh_.mesh, o, d);
    if (!h || h->face >= app_.mesh_.mesh.faces.size()) return;
    const Face& f = app_.mesh_.mesh.faces[h->face];
    const Corner* best = nullptr;
    float best_d = 1e30f;
    for (const auto& c : f.corners) {
        const float dd = glm::distance(app_.mesh_.mesh.positions[c.v], h->p);
        if (dd < best_d) { best_d = dd; best = &c; }
    }
    if (!best) return;
    if (app_.mode_ == InteractionMode::WeightPaint) {
        app_.wpaint_.weight = app_.mesh_.mesh.groups.empty() ? 0.0f : app_.mesh_.mesh.weight(best->v, std::min<uint32_t>(app_.active_group_, static_cast<uint32_t>(app_.mesh_.mesh.groups.size() - 1)));
    } else {
        app_.vpaint_.color = app_.mesh_.mesh.has_colors ? best->color : glm::vec4(1.0f);
    }
}

void PaintPanel::paint_fill_all_() {
    if (app_.mode_ == InteractionMode::WeightPaint) {
        const float w = app_.wpaint_.weight;
        app_.mesh_.edit("Set Weight", [&](EditMesh& m, MeshSelection&) { assign_weight(m, paint_group_(m), w); });
    } else {
        const glm::vec4 c = app_.vpaint_.color;
        app_.mesh_.edit("Set Vertex Colors", [&](EditMesh& m, MeshSelection&) { fill_vertex_colors(m, c); });
    }
}

void PaintPanel::handle_paint_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool shift = has(in.mods, Mods::Shift);
    const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
    if (!app_.paint_active_) return;
    PaintSettings& s = paint_settings_();
    if (app_.paint_resize_.active) {   // F / Shift+F, as in Sculpt Mode
        const float dx = m.x - app_.paint_resize_.start.x;
        if (app_.paint_resize_.strength) s.strength = std::clamp(app_.paint_resize_.start_value + dx / 300.0f, 0.0f, 1.0f);
        else s.radius_px = std::clamp(app_.paint_resize_.start_value + dx, 2.0f, 600.0f);
        bool cancel = in.pressed[1], confirm = in.pressed[0];
        for (const auto& e : in.keys) {
            if (e.action == coopa::input::KeyAction::Release) continue;
            if (e.key == Key::Escape) cancel = true;
            if (e.key == Key::Enter || e.key == Key::KpEnter) confirm = true;
        }
        ctx.consume_keyboard();
        if (cancel) {
            (app_.paint_resize_.strength ? s.strength : s.radius_px) = app_.paint_resize_.start_value;
            app_.paint_resize_.active = false;
        } else if (confirm) {
            app_.paint_resize_.active = false;
        }
        return;
    }
    if (app_.paint_stroke_.active) {
        if (!in.down[0]) { end_paint_stroke_(); return; }
        if (in.mouse_delta != glm::vec2(0.0f)) continue_paint_stroke_(vp, m);
        return;
    }
    if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt)) begin_paint_stroke_(vp, m, shift, ctrl);
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    app_.viewport_keymap_(ctx, vp, false, false);   // views, shading, sidebar... (no object edits)
    if (ctx.shortcut(Key::F)) app_.paint_resize_ = {true, false, m - glm::vec2(s.radius_px, 0.0f), s.radius_px};
    if (ctx.shortcut(Key::F, Mods::Shift)) app_.paint_resize_ = {true, true, m - glm::vec2(s.strength * 300.0f, 0.0f), s.strength};
    if (ctx.shortcut(Key::X) && app_.mode_ == InteractionMode::VertexPaint) std::swap(app_.vpaint_.color, app_.vpaint_.secondary);
    if (ctx.shortcut(Key::S)) paint_sample_(vp, m);
    if (ctx.shortcut(Key::K, Mods::Shift)) paint_fill_all_();
    if (ctx.shortcut(Key::Tab)) app_.exit_mesh_mode_();
}

void PaintPanel::draw_paint_overlay_(imm::Context& ctx, const ViewProj& vp) {
    if (!app_.paint_active_) return;
    const PaintSettings& s = paint_settings_();
    app_.sculpt_panel_->draw_brush_overlay_(ctx, vp, s.radius_px, s.strength, app_.paint_resize_, app_.paint_stroke_.active, app_.paint_stroke_.active && app_.paint_stroke_.invert);
}

float PaintPanel::draw_paint_header_(imm::Context& ctx, float x, const imm::Box& hb) {
    const float bh = hb.h - 6;
    PaintSettings& s = paint_settings_();
    auto label = [&](const char* t) {
        const float w = ctx.text_width(t) + 6;
        ctx.text_in({x, hb.y, w, hb.h}, t, ctx.style.text_dim, 0.0f);
        x += w;
    };
    if (app_.mode_ == InteractionMode::VertexPaint) {
        // Primary and secondary swatches (X swaps): click opens the colour picker.
        auto swatch = [&](const char* id, glm::vec4& c, const char* tip) {
            const imm::Box b{x, hb.y + 3, bh * 1.4f, bh};
            bool hov = false, held = false;
            const std::string pop = std::string(id) + "_pop";
            if (ctx.invisible_button(id, b, &hov, &held)) ctx.open_popup(pop, glm::vec2(b.x, b.bottom() + 2));
            ctx.fill_rounded(b, glm::vec4(glm::vec3(c), 1.0f));
            if (hov) ctx.fill_rounded(b, glm::vec4(1, 1, 1, 0.12f));
            ctx.tooltip(tip);
            if (ctx.begin_popup(pop, 230)) {
                ctx.color_edit("Color", &c.x, true);
                ctx.end_popup();
            }
            x += b.w + 3;
        };
        swatch("vp_color", s.color, "Color\nThe colour the brush paints (S samples it from the mesh)");
        swatch("vp_secondary", s.secondary, "Secondary Color\nCtrl paints with this one; X swaps the two");
        x += 6;
    } else {
        label("Weight");
        ctx.drag_float_box("wp_weight", {x, hb.y + 3, 52, bh}, &s.weight, 0.005f, 0.0f, 1.0f, "%.2f");
        ctx.tooltip("Weight\nThe weight the brush lays down in the active vertex group (S samples it)");
        x += 58;
    }
    label("Radius");
    ctx.drag_float_box("pt_radius", {x, hb.y + 3, 62, bh}, &s.radius_px, 0.5f, 2.0f, 600.0f, "%.0f px");
    ctx.tooltip("Radius\nBrush radius in screen pixels (F)");
    x += 68;
    label("Strength");
    ctx.drag_float_box("pt_strength", {x, hb.y + 3, 56, bh}, &s.strength, 0.005f, 0.0f, 1.0f, "%.2f");
    ctx.tooltip("Strength\nHow strongly each dab acts (Shift F)");
    x += 64;
    return x + 8;
}

void PaintPanel::draw_paint_toolbar_(imm::Context& ctx, const imm::Box& r) {
    using I = imm::Icon;
    float y = r.y + 4;
    PaintSettings& s = paint_settings_();
    struct B { PaintTool t; I ic; const char* tip; };
    static const B kTools[] = {
        {PaintTool::Draw, I::BrushDraw, "Draw\nPaint with the brush colour / weight (Ctrl inverts)"},
        {PaintTool::Blur, I::BrushSmooth, "Blur\nBlend each vertex with its neighbours; hold Shift with any tool"},
        {PaintTool::Average, I::BrushFlatten, "Average\nPull everything under the brush toward its average"},
    };
    for (const auto& b : kTools) {
        const imm::Box bb{r.x + 3, y, app_.viewport_chrome_panel_->kToolSize, app_.viewport_chrome_panel_->kToolSize};
        y += app_.viewport_chrome_panel_->kToolSize + 2;
        if (ctx.icon_button(std::string("ptool_") + paint_tool_name(b.t), b.ic, b.tip, s.tool == b.t, app_.viewport_chrome_panel_->kToolSize,
                            imm::Context::kAll, bb)) {
            s.tool = b.t;
        }
    }
}

void PaintPanel::draw_paint_menu_(imm::Context& ctx) {
    using I = imm::Icon;
    if (app_.mode_ == InteractionMode::VertexPaint) {
        if (!ctx.begin_menu("Paint")) return;
        if (ctx.menu_item("Set Vertex Colors", "Shift K", nullptr, true, I::VertexPaint)) paint_fill_all_();
        if (ctx.menu_item("Invert", "", nullptr, app_.mesh_.mesh.has_colors)) app_.mesh_.edit("Invert Colors", [](EditMesh& m, MeshSelection&) { invert_vertex_colors(m); });
        if (ctx.menu_item("Remove Color Attribute", "", nullptr, app_.mesh_.mesh.has_colors, I::Trash)) {
            app_.mesh_.edit("Remove Colors", [](EditMesh& m, MeshSelection&) { clear_vertex_colors(m); });
        }
        ctx.end_menu();
        return;
    }
    if (!ctx.begin_menu("Weights")) return;
    const bool any = !app_.mesh_.mesh.groups.empty();
    const bool group = app_.active_group_ < app_.mesh_.mesh.groups.size();
    if (ctx.menu_item("Set Weight", "Shift K", nullptr, true, I::WeightPaint)) paint_fill_all_();
    ctx.menu_separator();
    if (ctx.menu_item("Automatic Weights from Rig", "", nullptr, app_.edit_object_ != 0, I::WeightPaint)) {
        app_.weights_panel_->regenerate_weights_from_rig_(BindWeights::Automatic);
    }
    ctx.tooltip("Automatic Weights from Rig\nReplace the bone vertex groups with weights generated from the rig\n"
                "under this mesh (settings in Properties > Tool > Auto Weights)");
    if (ctx.menu_item("Envelope Weights from Rig", "", nullptr, app_.edit_object_ != 0)) {
        app_.weights_panel_->regenerate_weights_from_rig_(BindWeights::Envelope);
    }
    if (ctx.menu_item("Vertex Groups from Rig", "", nullptr, app_.edit_object_ != 0)) {
        app_.weights_panel_->regenerate_weights_from_rig_(BindWeights::EmptyGroups);
    }
    ctx.tooltip("Vertex Groups from Rig\nOne empty group per bone, leaving the weights to the brush");
    ctx.menu_separator();
    if (ctx.menu_item("Normalize All", "", nullptr, any)) app_.mesh_.edit("Normalize All", [](EditMesh& m, MeshSelection&) { normalize_all_weights(m); });
    if (ctx.menu_item("Limit Total", "", nullptr, any)) {
        app_.mesh_.edit("Limit Total", [](EditMesh& m, MeshSelection&) { limit_total_weights(m, 4); });
    }
    ctx.tooltip("Limit Total\nKeep each vertex's four strongest influences -- what the engine actually skins");
    if (ctx.menu_item("Clean", "", nullptr, any)) {
        app_.mesh_.edit("Clean Weights", [](EditMesh& m, MeshSelection&) { clean_weights(m, 0.01f); });
    }
    ctx.tooltip("Clean\nDrop weights under 0.01, which move a vertex too little to see");
    if (ctx.menu_item("Smooth", "", nullptr, any)) {
        app_.mesh_.edit("Smooth Weights", [this](EditMesh& m, MeshSelection&) { smooth_weights(m, app_.sculpt_cache_, std::nullopt, 0.5f, 1); });
    }
    ctx.tooltip("Smooth\nBlur every group toward each vertex's neighbours, softening a hard seam");
    if (ctx.begin_menu("Mirror", any)) {
        static const char* kAxes[] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a) {
            if (ctx.menu_item(kAxes[a])) {
                app_.mesh_.edit(std::string("Mirror Weights ") + kAxes[a],
                           [a](EditMesh& m, MeshSelection&) { mirror_weights(m, a); });
            }
            ctx.tooltip(std::string("Mirror ") + kAxes[a] +
                        "\nSwap each vertex's weights with the vertex opposite it across the mesh's " + kAxes[a] +
                        " axis,\nand side-named groups (Arm_L / Arm_R) with each other");
        }
        ctx.end_menu();
    }
    ctx.menu_separator();
    if (ctx.menu_item("Invert Active Group", "", nullptr, group)) {
        const uint32_t g = app_.active_group_;
        app_.mesh_.edit("Invert Group Weights", [g](EditMesh& m, MeshSelection&) { invert_group_weights(m, g); });
    }
    ctx.tooltip("Invert Active Group\nEvery vertex's weight becomes 1 minus what it was");
    if (ctx.menu_item("Clear Group", "", nullptr, any, I::Trash)) {
        const uint32_t g = app_.active_group_;
        app_.mesh_.edit("Clear Group Weights", [g](EditMesh& m, MeshSelection&) { assign_weight(m, g, 0.0f); });
    }
    ctx.end_menu();
}

void PaintPanel::draw_vertex_groups_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Vertex Groups", true, nullptr, I::WeightPaint)) return;
    EditMesh& m = app_.mesh_.mesh;
    if (m.groups.empty()) ctx.label_dim("No vertex groups.");
    for (uint32_t g = 0; g < m.groups.size(); ++g) {
        if (ctx.selectable(m.groups[g] + "##vg" + std::to_string(g), g == app_.active_group_, 0.0f, I::WeightPaint)) app_.active_group_ = g;
    }
    if (ctx.button("Add", -1, true, I::Plus)) {
        std::string name = "Group";
        for (int i = 1; std::find(m.groups.begin(), m.groups.end(), name) != m.groups.end(); ++i) name = "Group." + std::to_string(i);
        uint32_t added = 0;
        app_.mesh_.edit("Add Vertex Group", [&](EditMesh& mm, MeshSelection&) { added = mm.group_index(name); });
        app_.active_group_ = added;
    }
    if (!m.groups.empty()) {
        if (app_.active_group_ >= m.groups.size()) app_.active_group_ = static_cast<uint32_t>(m.groups.size() - 1);
        if (ctx.button("Remove", -1, true, I::Trash)) {
            const uint32_t g = app_.active_group_;
            app_.mesh_.edit("Remove Vertex Group", [g](EditMesh& mm, MeshSelection&) { mm.remove_group(g); });
            if (app_.active_group_ > 0) --app_.active_group_;
        }
        if (app_.active_group_ < m.groups.size()) {
            std::string name = m.groups[app_.active_group_];
            if (ctx.input_text("Name", &name) && !name.empty() &&
                std::find(m.groups.begin(), m.groups.end(), name) == m.groups.end()) {
                const uint32_t g = app_.active_group_;
                app_.mesh_.edit("Rename Vertex Group", [g, name](EditMesh& mm, MeshSelection&) { mm.groups[g] = name; }, "rename_vg");
            }
        }
    }
    if (app_.in_edit_mode_() && app_.active_group_ < m.groups.size()) {
        ctx.slider_float("Weight", &app_.edit_assign_weight_, 0.0f, 1.0f, "%.2f");
        const auto sel = app_.mesh_.selection.affected_vertices(m);
        const std::vector<uint32_t> verts(sel.begin(), sel.end());
        const uint32_t g = app_.active_group_;
        const float w = app_.edit_assign_weight_;
        if (ctx.button("Assign", -1, !verts.empty())) app_.mesh_.edit("Assign to Vertex Group", [&](EditMesh& mm, MeshSelection&) { assign_weight(mm, g, w, verts); });
        ctx.tooltip("Assign\nGive the selected vertices this weight in the active group");
        if (ctx.button("Remove from Group", -1, !verts.empty())) app_.mesh_.edit("Remove from Vertex Group", [&](EditMesh& mm, MeshSelection&) { assign_weight(mm, g, 0.0f, verts); });
        if (ctx.button("Select", -1)) {
            for (uint32_t v = 0; v < m.positions.size(); ++v) if (m.weight(v, g) > 0.0f) app_.mesh_.selection.verts.insert(v);
            app_.mesh_.selection.mode = SelectMode::Vertex;
            app_.mesh_.selection.validate(m);
        }
        ctx.tooltip("Select\nAdd the active group's vertices to the selection");
    }
}

void PaintPanel::draw_paint_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    PaintSettings& s = paint_settings_();
    const bool weights = app_.mode_ == InteractionMode::WeightPaint;
    if (ctx.collapsing_header("Brush", true, nullptr, weights ? I::WeightPaint : I::VertexPaint)) {
        int t = static_cast<int>(s.tool);
        if (ctx.combo("Tool", &t, {"Draw", "Blur", "Average"})) s.tool = static_cast<PaintTool>(t);
        int b = static_cast<int>(s.blend);
        if (weights) {
            if (b > 2) b = 0;
            if (ctx.combo("Blend", &b, {"Mix", "Add", "Subtract"})) s.blend = static_cast<PaintBlend>(b);
            ctx.slider_float("Weight", &s.weight, 0.0f, 1.0f, "%.2f");
        } else {
            if (ctx.combo("Blend", &b, {"Mix", "Add", "Subtract", "Multiply", "Lighten", "Darken"})) s.blend = static_cast<PaintBlend>(b);
            ctx.color_edit("Color", &s.color.x, true);
            ctx.color_edit("Secondary", &s.secondary.x, true);
        }
        ctx.drag_float("Radius (px)", &s.radius_px, 0.5f, 2.0f, 600.0f);
        ctx.slider_float("Strength", &s.strength, 0.0f, 1.0f, "%.2f");
        ctx.slider_float("Spacing", &s.spacing, 0.02f, 1.0f, "%.2f");
        ctx.checkbox("Front Faces Only", &s.front_faces_only);
        ctx.tooltip("Front Faces Only\nSkip vertices facing away from the view, so a stroke does not bleed through to the far side");
        if (weights) {
            ctx.checkbox("Auto Normalize", &s.auto_normalize);
            ctx.tooltip("Auto Normalize\nRescale each painted vertex's other groups so its weights sum to 1");
        }
        app_.sculpt_panel_->draw_symmetry_panel_(ctx, s.symmetry);
    }
    if (weights) {
        draw_vertex_groups_panel_(ctx);
        app_.weights_panel_->draw_auto_weights_panel_(ctx);
    } else if (ctx.collapsing_header("Color Attribute", true, nullptr, I::VertexPaint)) {
        ctx.label_dim(app_.mesh_.mesh.has_colors ? "The mesh has vertex colours (saved as `colors`)." : "No colours yet: the first stroke adds them (white).");
        if (ctx.button("Fill with Color", -1, true, I::VertexPaint)) paint_fill_all_();
        if (ctx.button("Remove Colors", -1, app_.mesh_.mesh.has_colors, I::Trash)) {
            app_.mesh_.edit("Remove Colors", [](EditMesh& m, MeshSelection&) { clear_vertex_colors(m); });
        }
    }
}

std::string PaintPanel::edited_mesh_shader() {
    auto rs = edited_mesh_renderers_();
    return rs.empty() ? std::string() : rs.front().second->material.shader;
}

} // namespace editor
} // namespace toy
