#include "editor/app/ui/sculpt.h"
#include "editor/app/ui/paint.h"

#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

float SculptPanel::sculpt_local_radius_(const ViewProj& vp, const glm::vec3& local_hit) {
    return app_.paint_panel_->brush_local_radius_(vp, local_hit, app_.sculpt_.radius_px);
}

SculptSettings& SculptPanel::sculpt_settings() {
    return app_.sculpt_;
}

MirrorSettings& SculptPanel::edit_symmetry() {
    return app_.edit_symmetry_;
}

bool SculptPanel::sculpt_stroking() const {
    return app_.stroke_.active;
}

void SculptPanel::sculpt_begin_() {
    if (app_.mesh_.mesh.faces.empty()) return;
    try {
        app_.sculpt_cache_.build(app_.mesh_.mesh);
        app_.sculpt_preview_.build(app_.engine_.device(), app_.engine_.allocator(), app_.mesh_.mesh, app_.sculpt_cache_);
    } catch (const std::exception& e) {
        app_.log_error(std::string("Sculpt Mode: ") + e.what());
        app_.sculpt_active_ = false;
        return;
    }
    app_.sculpt_active_ = true;
    app_.sculpt_cache_geom_ = app_.mesh_.geometry_revision;
    app_.sculpt_preview_pos_ = app_.mesh_.position_revision;
    assign_sculpt_preview_();
    app_.tool_ = EditorApp::Tool::Select;
    if (app_.mesh_.mesh.faces.size() < 2000) {
        app_.log_info("Sculpt Mode: this mesh has few faces -- Subdivide (Tool tab) adds detail to sculpt");
    }
}

void SculptPanel::sculpt_end_() {
    if (app_.stroke_.active) end_stroke_();
    app_.sculpt_resize_.active = false;
    if (!app_.sculpt_active_) return;
    app_.sculpt_active_ = false;
    app_.sculpt_preview_.reset();
    app_.scene_uploaded_revision_ = 0;   // push_mesh_to_scene_ re-exports the sculpted mesh once
    app_.uploaded_revision_ = 0;         // and the mesh viewer re-uploads it
}

void SculptPanel::assign_sculpt_preview_() {
    if (!app_.sculpt_preview_.mesh()) return;
    auto handle = app_.engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/sculpt_mesh", app_.sculpt_preview_.mesh());
    if (app_.active_type_ == AssetType::Mesh) {   // the mesh asset's own viewer
        if (auto* mr = app_.preview_renderer_()) mr->set_mesh(handle);
        return;
    }
    const fs::path dir = (app_.doc_.path().empty() ? app_.sync_.fallback_path : app_.doc_.path()).parent_path();
    for (const auto& [id, live] : app_.sync_.live_objects()) {
        const int ci = app_.doc_.find_component(id, "MeshRenderer");
        if (ci < 0 || !live) continue;
        const std::string key = app_.object_mesh_key_(app_.instances_panel_->effective_(*app_.doc_.find(id)));   // a skinned mesh's too
        const fs::path p = coopa::yaml::resolve_variant(app_.engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
        if (p != app_.mesh_.path) continue;
        if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) mr->set_mesh(handle);
    }
    app_.mesh_cache_.clear();
    app_.scene_uploaded_revision_ = app_.mesh_.geometry_revision;
}

void SculptPanel::sculpt_frame_() {
    if (!app_.sculpt_active_) return;
    if (!app_.mesh_.live() && app_.sculpt_cache_geom_ != app_.mesh_.geometry_revision) {
        const bool same_topology = SculptCache::topology_key(app_.mesh_.mesh) == app_.sculpt_cache_.topo_key;
        try {
            app_.sculpt_cache_.build(app_.mesh_.mesh);
            if (!same_topology) {
                app_.sculpt_preview_.build(app_.engine_.device(), app_.engine_.allocator(), app_.mesh_.mesh, app_.sculpt_cache_);
                assign_sculpt_preview_();
            }
        } catch (const std::exception& e) {
            app_.log_error(std::string("Sculpt Mode: ") + e.what());
        }
        app_.sculpt_cache_geom_ = app_.mesh_.geometry_revision;
        app_.sculpt_preview_pos_ = 0;
    }
    app_.scene_uploaded_revision_ = app_.mesh_.geometry_revision;   // the preview shows it; no re-export
    if (app_.sculpt_preview_pos_ != app_.mesh_.position_revision) {
        app_.sculpt_preview_.update(app_.mesh_.mesh, app_.sculpt_cache_, app_.engine_.frame_slot());
        app_.sculpt_preview_pos_ = app_.mesh_.position_revision;
    }
}

bool SculptPanel::sculpt_hit_(const ViewProj& vp, glm::vec2 px, glm::vec3& hit, glm::vec3& normal) {
    glm::vec3 o, d;
    app_.mesh_tools_panel_->mesh_local_ray_(vp, px, o, d);
    auto h = app_.sculpt_cache_.bvh.raycast(app_.mesh_.mesh, o, d);
    if (!h) return false;
    hit = h->p;
    normal = glm::length(h->normal) > 1e-12f ? glm::normalize(h->normal) : glm::vec3(0, 0, 1);
    if (glm::dot(normal, d) > 0.0f) normal = -normal;
    return true;
}

void SculptPanel::sculpt_dab_at_(const ViewProj& vp, glm::vec2 px, std::vector<uint32_t>& moved) {
    glm::vec3 hit, n;
    if (!sculpt_hit_(vp, px, hit, n)) return;
    if (app_.sculpt_grid_.stale() || std::abs(app_.sculpt_grid_.cell() - app_.stroke_.radius) > app_.stroke_.radius * 0.5f) {
        app_.sculpt_grid_.build(app_.mesh_.mesh, app_.stroke_.radius);
    }
    SculptDab d;
    d.center = hit;
    d.view_dir = -n;
    d.radius = app_.stroke_.radius;
    d.strength = app_.sculpt_.strength;
    d.invert = app_.stroke_.invert;
    d.brush = app_.stroke_.brush;
    float maxd = 0.0f;
    for_each_symmetric(d, mirror_images(app_.sculpt_.symmetry, app_.mesh_world_()), [&](const SculptDab& md) {
        maxd = std::max(maxd, sculpt_dab(app_.mesh_.mesh, app_.sculpt_cache_, app_.sculpt_grid_, md, moved));
    });
    app_.sculpt_grid_.note_moved(maxd);
}

void SculptPanel::begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl) {
    glm::vec3 hit, n;
    if (!sculpt_hit_(vp, px, hit, n)) return;
    app_.stroke_ = Stroke{};
    app_.stroke_.active = true;
    app_.stroke_.brush = shift ? SculptBrush::Smooth : app_.sculpt_.brush;
    app_.stroke_.invert = ctrl;
    app_.stroke_.radius = std::max(1e-5f, sculpt_local_radius_(vp, hit));
    app_.stroke_.last_px = px;
    app_.mesh_.begin_live();
    app_.sculpt_grid_.build(app_.mesh_.mesh, app_.stroke_.radius);
    std::vector<uint32_t> moved;
    if (app_.stroke_.brush == SculptBrush::Grab) {
        const glm::mat4 w = app_.mesh_world_();
        app_.stroke_.start_world = glm::vec3(w * glm::vec4(hit, 1.0f));
        app_.stroke_.plane_point = app_.stroke_.start_world;
        app_.stroke_.plane_normal = vp.camera_forward();
        SculptDab d;
        d.center = hit;
        d.radius = app_.stroke_.radius;
        for_each_symmetric(d, mirror_images(app_.sculpt_.symmetry, app_.mesh_world_()), [&](const SculptDab& md) {
            app_.stroke_.grabs.push_back(sculpt_grab_begin(app_.mesh_.mesh, app_.sculpt_grid_, md.center, app_.stroke_.radius, app_.sculpt_.strength, md.mirror));
        });
        return;
    }
    sculpt_dab_at_(vp, px, moved);
    finish_dabs_(moved);
}

void SculptPanel::continue_stroke_(const ViewProj& vp, glm::vec2 px) {
    std::vector<uint32_t> moved;
    if (app_.stroke_.brush == SculptBrush::Grab) {
        glm::vec3 o, d;
        vp.ray(px, o, d);
        const float den = glm::dot(d, app_.stroke_.plane_normal);
        if (std::abs(den) < 1e-6f) return;
        const glm::vec3 p = o + d * (glm::dot(app_.stroke_.plane_point - o, app_.stroke_.plane_normal) / den);
        const glm::vec3 local_delta = glm::vec3(glm::inverse(app_.mesh_world_()) * glm::vec4(p - app_.stroke_.start_world, 0.0f));
        for (const auto& g : app_.stroke_.grabs) sculpt_grab_apply(app_.mesh_.mesh, g, local_delta, moved);
        finish_dabs_(moved);
        return;
    }
    const float step = std::max(1.0f, app_.sculpt_.spacing * app_.sculpt_.radius_px);
    glm::vec2 cur = app_.stroke_.last_px;
    float dist = glm::distance(px, cur);
    int guard = 0;
    while (dist >= step && guard++ < 256) {
        cur += (px - cur) / dist * step;
        sculpt_dab_at_(vp, cur, moved);
        dist = glm::distance(px, cur);
    }
    app_.stroke_.last_px = cur;
    finish_dabs_(moved);
}

void SculptPanel::finish_dabs_(const std::vector<uint32_t>& moved) {
    if (moved.empty()) return;
    app_.sculpt_cache_.update_normals(app_.mesh_.mesh, moved);
    app_.sculpt_cache_.bvh.refit(app_.mesh_.mesh);
    app_.mesh_.touch_live();
}

void SculptPanel::end_stroke_() {
    if (!app_.stroke_.active) return;
    app_.stroke_.active = false;
    app_.mesh_.end_live(std::string("Sculpt ") + sculpt_brush_name(app_.stroke_.brush));
    app_.sculpt_cache_geom_ = app_.mesh_.geometry_revision;   // the cache followed the stroke incrementally
}

void SculptPanel::handle_sculpt_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool shift = has(in.mods, Mods::Shift);
    const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
    if (!app_.sculpt_active_) return;
    // F / Shift+F radius & strength.
    if (app_.sculpt_resize_.active) {
        const float dx = m.x - app_.sculpt_resize_.start.x;
        if (app_.sculpt_resize_.strength) app_.sculpt_.strength = std::clamp(app_.sculpt_resize_.start_value + dx / 300.0f, 0.0f, 1.0f);
        else app_.sculpt_.radius_px = std::clamp(app_.sculpt_resize_.start_value + dx, 2.0f, 600.0f);
        bool cancel = in.pressed[1], confirm = in.pressed[0];
        for (const auto& e : in.keys) {
            if (e.action == coopa::input::KeyAction::Release) continue;
            if (e.key == Key::Escape) cancel = true;
            if (e.key == Key::Enter || e.key == Key::KpEnter) confirm = true;
        }
        ctx.consume_keyboard();
        if (cancel) {
            (app_.sculpt_resize_.strength ? app_.sculpt_.strength : app_.sculpt_.radius_px) = app_.sculpt_resize_.start_value;
            app_.sculpt_resize_.active = false;
        } else if (confirm) {
            app_.sculpt_resize_.active = false;
        }
        return;
    }
    if (app_.stroke_.active) {
        if (!in.down[0]) { end_stroke_(); return; }
        if (in.mouse_delta != glm::vec2(0.0f)) continue_stroke_(vp, m);
        return;
    }
    if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt)) begin_stroke_(vp, m, shift, ctrl);
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    app_.viewport_keymap_(ctx, vp, false, false);   // views, shading, sidebar... (no object edits)
    if (ctx.shortcut(Key::F)) app_.sculpt_resize_ = {true, false, m - glm::vec2(app_.sculpt_.radius_px, 0.0f), app_.sculpt_.radius_px};
    if (ctx.shortcut(Key::F, Mods::Shift)) app_.sculpt_resize_ = {true, true, m - glm::vec2(app_.sculpt_.strength * 300.0f, 0.0f), app_.sculpt_.strength};
    // Blender's brush hotkeys.
    if (ctx.shortcut(Key::X)) app_.sculpt_.brush = SculptBrush::Draw;
    if (ctx.shortcut(Key::S)) app_.sculpt_.brush = SculptBrush::Smooth;
    if (ctx.shortcut(Key::I)) app_.sculpt_.brush = SculptBrush::Inflate;
    if (ctx.shortcut(Key::G)) app_.sculpt_.brush = SculptBrush::Grab;
    if (ctx.shortcut(Key::T, Mods::Shift)) app_.sculpt_.brush = SculptBrush::Flatten;
    if (ctx.shortcut(Key::Tab)) app_.exit_mesh_mode_();
}

void SculptPanel::draw_sculpt_overlay_(imm::Context& ctx, const ViewProj& vp) {
    if (!app_.sculpt_active_) return;
    draw_brush_overlay_(ctx, vp, app_.sculpt_.radius_px, app_.sculpt_.strength, app_.sculpt_resize_, app_.stroke_.active, app_.stroke_.active && app_.stroke_.invert);
}

void SculptPanel::draw_brush_overlay_(imm::Context& ctx, const ViewProj& vp, float radius_px, float strength, const SculptResize& resize,
                         bool stroking, bool inverted) {
    const glm::vec2 m = ctx.mouse();
    const glm::vec4 col = inverted ? glm::vec4(0.55f, 0.75f, 1.0f, 0.9f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.85f);
    if (resize.active) {
        const glm::vec2 c = resize.start;
        const float r = radius_px;
        for (int i = 0; i < 64; ++i) {
            const float a0 = 6.2831853f * i / 64, a1 = 6.2831853f * (i + 1) / 64;
            ctx.line(c + glm::vec2(std::cos(a0), std::sin(a0)) * r, c + glm::vec2(std::cos(a1), std::sin(a1)) * r, col, 1.5f);
        }
        if (resize.strength) ctx.circle(c, std::max(2.0f, r * strength), imm::with_alpha(ctx.style.accent, 0.35f));
        char buf[64];
        if (resize.strength) std::snprintf(buf, sizeof(buf), "Strength %.2f", strength);
        else std::snprintf(buf, sizeof(buf), "Radius %.0f px", radius_px);
        app_.viewport_chrome_panel_->shadow_text_(ctx, c + glm::vec2(-30, -8), buf, app_.et_.viewport.overlay_text);
        return;
    }
    if (!app_.viewport_hovered_ && !stroking) return;
    glm::vec3 hit, n;
    if (sculpt_hit_(vp, m, hit, n)) {
        const glm::mat4 w = app_.mesh_world_();
        const float r = app_.paint_panel_->brush_local_radius_(vp, hit, radius_px);
        const glm::vec3 u = glm::normalize(glm::cross(n, std::abs(n.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0)));
        const glm::vec3 v = glm::cross(n, u);
        std::optional<glm::vec2> prev;
        for (int i = 0; i <= 48; ++i) {
            const float a = 6.2831853f * i / 48;
            const glm::vec3 p = hit + (u * std::cos(a) + v * std::sin(a)) * r + n * (r * 0.02f);
            auto q = vp.project(glm::vec3(w * glm::vec4(p, 1.0f)));
            if (prev && q) ctx.line(*prev, *q, col, 1.5f);
            prev = q;
        }
        // The inner ring shows the strength.
        for (int i = 0; i < 32; ++i) {
            const float a0 = 6.2831853f * i / 32, a1 = 6.2831853f * (i + 1) / 32;
            const float rr = r * std::max(0.08f, strength * 0.6f);
            auto qa = vp.project(glm::vec3(w * glm::vec4(hit + (u * std::cos(a0) + v * std::sin(a0)) * rr, 1.0f)));
            auto qb = vp.project(glm::vec3(w * glm::vec4(hit + (u * std::cos(a1) + v * std::sin(a1)) * rr, 1.0f)));
            if (qa && qb) ctx.line(*qa, *qb, imm::with_alpha(col, 0.4f), 1.0f);
        }
    } else {
        const float r = radius_px;
        for (int i = 0; i < 48; ++i) {
            const float a0 = 6.2831853f * i / 48, a1 = 6.2831853f * (i + 1) / 48;
            ctx.line(m + glm::vec2(std::cos(a0), std::sin(a0)) * r, m + glm::vec2(std::cos(a1), std::sin(a1)) * r,
                     imm::with_alpha(col, 0.45f), 1.0f);
        }
    }
}

float SculptPanel::draw_sculpt_header_(imm::Context& ctx, float x, const imm::Box& hb) {
    using I = imm::Icon;
    const float bh = hb.h - 6;
    auto label = [&](const char* t) {
        const float w = ctx.text_width(t) + 6;
        ctx.text_in({x, hb.y, w, hb.h}, t, ctx.style.text_dim, 0.0f);
        x += w;
    };
    label("Radius");
    ctx.drag_float_box("sc_radius", {x, hb.y + 3, 62, bh}, &app_.sculpt_.radius_px, 0.5f, 2.0f, 600.0f, "%.0f px");
    ctx.tooltip("Radius\nBrush radius in screen pixels (F)");
    x += 68;
    label("Strength");
    ctx.drag_float_box("sc_strength", {x, hb.y + 3, 56, bh}, &app_.sculpt_.strength, 0.005f, 0.0f, 1.0f, "%.2f");
    ctx.tooltip("Strength\nHow strongly each dab acts (Shift F)");
    x += 64;
    return x + 8;   // symmetry sits after the menus (draw_viewport_header_), space permitting
}

std::string SculptPanel::symmetry_label_(const MirrorSettings& sym) {
    std::string l = "Mirror";
    if (!sym.any()) return l;
    l += ' ';
    for (int a = 0; a < 3; ++a) if (sym.axis[a]) l += "XYZ"[a];
    if (sym.global) l += " (Global)";
    return l;
}

float SculptPanel::draw_symmetry_buttons_(imm::Context& ctx, float x, const imm::Box& hb, MirrorSettings& sym, const char* id,
                             const char* what) {
    const float bh = hb.h - 6;
    const std::string label = symmetry_label_(sym);
    const imm::Box b{x, hb.y + 3, symmetry_buttons_width_(ctx, hb, sym), bh};
    bool hov = false, held = false;
    const std::string popup = std::string(id) + "_pop";
    if (ctx.invisible_button(id, b, &hov, &held)) ctx.open_popup(popup, glm::vec2(b.x, b.bottom() + 2));
    ctx.fill_rounded(b, sym.any() ? ctx.style.accent : hov ? ctx.style.button_hover : ctx.style.button);
    ctx.text_in({b.x + 6, b.y, b.w - bh, bh}, label, ctx.style.text, 0.0f);
    ctx.arrow({b.right() - 14, b.y + 4, 10, bh - 8}, true, ctx.style.text_dim);
    ctx.tooltip(std::string("Mirror\nRepeat ") + what + " across the chosen axes, the mesh's own (Local) or the world's (Global)");
    if (ctx.begin_popup(popup, 200)) {
        ctx.label_dim(std::string("Mirror ") + what);
        draw_symmetry_panel_(ctx, sym);
        ctx.end_popup();
    }
    return x + b.w;
}

void SculptPanel::draw_symmetry_panel_(imm::Context& ctx, MirrorSettings& sym) {
    ctx.checkbox("Mirror X", &sym.axis[0]);
    ctx.checkbox("Mirror Y", &sym.axis[1]);
    ctx.checkbox("Mirror Z", &sym.axis[2]);
    int space = sym.global ? 1 : 0;
    if (ctx.combo("Mirror Axes", &space, {"Local", "Global"})) sym.global = space == 1;
    ctx.tooltip("Mirror Axes\nLocal: the mesh's own axes through its origin. Global: the world's axes through the world origin.");
}

void SculptPanel::draw_sculpt_toolbar_(imm::Context& ctx, const imm::Box& r) {
    using I = imm::Icon;
    float y = r.y + 4;
    struct B { SculptBrush b; I ic; const char* tip; };
    static const B kBrushes[] = {
        {SculptBrush::Draw, I::BrushDraw, "Draw\nPull the surface out along its normal (X); Ctrl pushes in"},
        {SculptBrush::Smooth, I::BrushSmooth, "Smooth\nEven out bumps (S); hold Shift with any brush"},
        {SculptBrush::Inflate, I::BrushInflate, "Inflate\nSwell along each vertex's normal (I); Ctrl deflates"},
        {SculptBrush::Grab, I::BrushGrab, "Grab\nDrag the vertices under the brush with the cursor (G)"},
        {SculptBrush::Flatten, I::BrushFlatten, "Flatten\nPress the surface onto its average plane (Shift T)"},
    };
    for (const auto& b : kBrushes) {
        const imm::Box bb{r.x + 3, y, app_.viewport_chrome_panel_->kToolSize, app_.viewport_chrome_panel_->kToolSize};
        y += app_.viewport_chrome_panel_->kToolSize + 2;
        if (ctx.icon_button(std::string("brush_") + sculpt_brush_name(b.b), b.ic, b.tip, app_.sculpt_.brush == b.b, app_.viewport_chrome_panel_->kToolSize,
                            imm::Context::kAll, bb)) {
            app_.sculpt_.brush = b.b;
        }
    }
}

void SculptPanel::draw_sculpt_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.collapsing_header("Brush", true, nullptr, I::SculptMode)) {
        int b = static_cast<int>(app_.sculpt_.brush);
        if (ctx.combo("Brush", &b, {"Draw", "Smooth", "Inflate", "Grab", "Flatten"})) app_.sculpt_.brush = static_cast<SculptBrush>(b);
        ctx.drag_float("Radius (px)", &app_.sculpt_.radius_px, 0.5f, 2.0f, 600.0f);
        ctx.slider_float("Strength", &app_.sculpt_.strength, 0.0f, 1.0f, "%.2f");
        ctx.slider_float("Spacing", &app_.sculpt_.spacing, 0.02f, 1.0f, "%.2f");
        draw_symmetry_panel_(ctx, app_.sculpt_.symmetry);
    }
    if (ctx.collapsing_header("Density", true, nullptr, I::Mesh)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%zu vertices, %zu faces", app_.mesh_.mesh.positions.size(), app_.mesh_.mesh.faces.size());
        ctx.label_dim(buf);
        const bool big = app_.mesh_.mesh.faces.size() * 4 > 600000;
        if (ctx.button("Subdivide Smooth", -1, !big, I::Plus)) app_.mesh_tools_panel_->run_catmull_clark_(1);
        ctx.tooltip("Subdivide Smooth\nCatmull-Clark: every face becomes quads and the surface rounds off");
        if (ctx.button("Subdivide Simple", -1, !big, I::Plus)) {
            MeshSelection all;
            all.select_all(app_.mesh_.mesh);
            app_.mesh_.selection = all;
            app_.mesh_tools_panel_->run_subdivide_(1);
        }
        ctx.tooltip("Subdivide Simple\nSplit every face without changing the shape");
        if (big) ctx.label_dim("Already dense: subdividing would pass 600k faces.");
        bool smooth = !app_.mesh_.mesh.faces.empty() && app_.mesh_.mesh.faces[0].smooth;
        if (ctx.checkbox("Shade Smooth", &smooth)) {
            app_.mesh_.edit(smooth ? "Shade Smooth" : "Shade Flat", [&](EditMesh& mm, MeshSelection&) {
                for (auto& f : mm.faces) f.smooth = smooth;
            });
        }
        ctx.tooltip("Shade Smooth\nInterpolate normals across faces (the whole mesh)");
    }
}

} // namespace editor
} // namespace toy
