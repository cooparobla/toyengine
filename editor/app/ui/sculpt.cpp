#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

void EditorApp::sculpt_begin_() {
    if (mesh_.mesh.faces.empty()) return;
    try {
        sculpt_cache_.build(mesh_.mesh);
        sculpt_preview_.build(engine_.device(), engine_.allocator(), mesh_.mesh, sculpt_cache_);
    } catch (const std::exception& e) {
        log_error(std::string("Sculpt Mode: ") + e.what());
        sculpt_active_ = false;
        return;
    }
    sculpt_active_ = true;
    sculpt_cache_geom_ = mesh_.geometry_revision;
    sculpt_preview_pos_ = mesh_.position_revision;
    assign_sculpt_preview_();
    tool_ = Tool::Select;
    if (mesh_.mesh.faces.size() < 2000) {
        log_info("Sculpt Mode: this mesh has few faces -- Subdivide (Tool tab) adds detail to sculpt");
    }
}

void EditorApp::sculpt_end_() {
    if (stroke_.active) end_stroke_();
    sculpt_resize_.active = false;
    if (!sculpt_active_) return;
    sculpt_active_ = false;
    sculpt_preview_.reset();
    scene_uploaded_revision_ = 0;   // push_mesh_to_scene_ re-exports the sculpted mesh once
    uploaded_revision_ = 0;         // and the mesh viewer re-uploads it
}

void EditorApp::assign_sculpt_preview_() {
    if (!sculpt_preview_.mesh()) return;
    auto handle = engine_.assets().create<coopa::gfx::engine::data::Mesh>("editor/sculpt_mesh", sculpt_preview_.mesh());
    if (active_type_ == AssetType::Mesh) {   // the mesh asset's own viewer
        if (auto* mr = preview_renderer_()) mr->set_mesh(handle);
        return;
    }
    const fs::path dir = (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path();
    for (const auto& [id, live] : sync_.live_objects()) {
        const int ci = doc_.find_component(id, "MeshRenderer");
        if (ci < 0 || !live) continue;
        const std::string key = object_mesh_key_(effective_(*doc_.find(id)));   // a skinned mesh's too
        const fs::path p = coopa::yaml::resolve_variant(engine_.assets().source().resolve("meshes/" + key + ".yaml", dir.string()));
        if (p != mesh_.path) continue;
        if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) mr->set_mesh(handle);
    }
    mesh_cache_.clear();
    scene_uploaded_revision_ = mesh_.geometry_revision;
}

void EditorApp::sculpt_frame_() {
    if (!sculpt_active_) return;
    if (!mesh_.live() && sculpt_cache_geom_ != mesh_.geometry_revision) {
        const bool same_topology = SculptCache::topology_key(mesh_.mesh) == sculpt_cache_.topo_key;
        try {
            sculpt_cache_.build(mesh_.mesh);
            if (!same_topology) {
                sculpt_preview_.build(engine_.device(), engine_.allocator(), mesh_.mesh, sculpt_cache_);
                assign_sculpt_preview_();
            }
        } catch (const std::exception& e) {
            log_error(std::string("Sculpt Mode: ") + e.what());
        }
        sculpt_cache_geom_ = mesh_.geometry_revision;
        sculpt_preview_pos_ = 0;
    }
    scene_uploaded_revision_ = mesh_.geometry_revision;   // the preview shows it; no re-export
    if (sculpt_preview_pos_ != mesh_.position_revision) {
        sculpt_preview_.update(mesh_.mesh, sculpt_cache_, engine_.frame_slot());
        sculpt_preview_pos_ = mesh_.position_revision;
    }
}

bool EditorApp::sculpt_hit_(const ViewProj& vp, glm::vec2 px, glm::vec3& hit, glm::vec3& normal) {
    glm::vec3 o, d;
    mesh_local_ray_(vp, px, o, d);
    auto h = sculpt_cache_.bvh.raycast(mesh_.mesh, o, d);
    if (!h) return false;
    hit = h->p;
    normal = glm::length(h->normal) > 1e-12f ? glm::normalize(h->normal) : glm::vec3(0, 0, 1);
    if (glm::dot(normal, d) > 0.0f) normal = -normal;
    return true;
}

void EditorApp::sculpt_dab_at_(const ViewProj& vp, glm::vec2 px, std::vector<uint32_t>& moved) {
    glm::vec3 hit, n;
    if (!sculpt_hit_(vp, px, hit, n)) return;
    if (sculpt_grid_.stale() || std::abs(sculpt_grid_.cell() - stroke_.radius) > stroke_.radius * 0.5f) {
        sculpt_grid_.build(mesh_.mesh, stroke_.radius);
    }
    SculptDab d;
    d.center = hit;
    d.view_dir = -n;
    d.radius = stroke_.radius;
    d.strength = sculpt_.strength;
    d.invert = stroke_.invert;
    d.brush = stroke_.brush;
    float maxd = 0.0f;
    for_each_symmetric(d, mirror_images(sculpt_.symmetry, mesh_world_()), [&](const SculptDab& md) {
        maxd = std::max(maxd, sculpt_dab(mesh_.mesh, sculpt_cache_, sculpt_grid_, md, moved));
    });
    sculpt_grid_.note_moved(maxd);
}

void EditorApp::begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl) {
    glm::vec3 hit, n;
    if (!sculpt_hit_(vp, px, hit, n)) return;
    stroke_ = Stroke{};
    stroke_.active = true;
    stroke_.brush = shift ? SculptBrush::Smooth : sculpt_.brush;
    stroke_.invert = ctrl;
    stroke_.radius = std::max(1e-5f, sculpt_local_radius_(vp, hit));
    stroke_.last_px = px;
    mesh_.begin_live();
    sculpt_grid_.build(mesh_.mesh, stroke_.radius);
    std::vector<uint32_t> moved;
    if (stroke_.brush == SculptBrush::Grab) {
        const glm::mat4 w = mesh_world_();
        stroke_.start_world = glm::vec3(w * glm::vec4(hit, 1.0f));
        stroke_.plane_point = stroke_.start_world;
        stroke_.plane_normal = vp.camera_forward();
        SculptDab d;
        d.center = hit;
        d.radius = stroke_.radius;
        for_each_symmetric(d, mirror_images(sculpt_.symmetry, mesh_world_()), [&](const SculptDab& md) {
            stroke_.grabs.push_back(sculpt_grab_begin(mesh_.mesh, sculpt_grid_, md.center, stroke_.radius, sculpt_.strength, md.mirror));
        });
        return;
    }
    sculpt_dab_at_(vp, px, moved);
    finish_dabs_(moved);
}

void EditorApp::continue_stroke_(const ViewProj& vp, glm::vec2 px) {
    std::vector<uint32_t> moved;
    if (stroke_.brush == SculptBrush::Grab) {
        glm::vec3 o, d;
        vp.ray(px, o, d);
        const float den = glm::dot(d, stroke_.plane_normal);
        if (std::abs(den) < 1e-6f) return;
        const glm::vec3 p = o + d * (glm::dot(stroke_.plane_point - o, stroke_.plane_normal) / den);
        const glm::vec3 local_delta = glm::vec3(glm::inverse(mesh_world_()) * glm::vec4(p - stroke_.start_world, 0.0f));
        for (const auto& g : stroke_.grabs) sculpt_grab_apply(mesh_.mesh, g, local_delta, moved);
        finish_dabs_(moved);
        return;
    }
    const float step = std::max(1.0f, sculpt_.spacing * sculpt_.radius_px);
    glm::vec2 cur = stroke_.last_px;
    float dist = glm::distance(px, cur);
    int guard = 0;
    while (dist >= step && guard++ < 256) {
        cur += (px - cur) / dist * step;
        sculpt_dab_at_(vp, cur, moved);
        dist = glm::distance(px, cur);
    }
    stroke_.last_px = cur;
    finish_dabs_(moved);
}

void EditorApp::finish_dabs_(const std::vector<uint32_t>& moved) {
    if (moved.empty()) return;
    sculpt_cache_.update_normals(mesh_.mesh, moved);
    sculpt_cache_.bvh.refit(mesh_.mesh);
    mesh_.touch_live();
}

void EditorApp::end_stroke_() {
    if (!stroke_.active) return;
    stroke_.active = false;
    mesh_.end_live(std::string("Sculpt ") + sculpt_brush_name(stroke_.brush));
    sculpt_cache_geom_ = mesh_.geometry_revision;   // the cache followed the stroke incrementally
}

void EditorApp::handle_sculpt_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool shift = has(in.mods, Mods::Shift);
    const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
    if (!sculpt_active_) return;
    // F / Shift+F radius & strength.
    if (sculpt_resize_.active) {
        const float dx = m.x - sculpt_resize_.start.x;
        if (sculpt_resize_.strength) sculpt_.strength = std::clamp(sculpt_resize_.start_value + dx / 300.0f, 0.0f, 1.0f);
        else sculpt_.radius_px = std::clamp(sculpt_resize_.start_value + dx, 2.0f, 600.0f);
        bool cancel = in.pressed[1], confirm = in.pressed[0];
        for (const auto& e : in.keys) {
            if (e.action == coopa::input::KeyAction::Release) continue;
            if (e.key == Key::Escape) cancel = true;
            if (e.key == Key::Enter || e.key == Key::KpEnter) confirm = true;
        }
        ctx.consume_keyboard();
        if (cancel) {
            (sculpt_resize_.strength ? sculpt_.strength : sculpt_.radius_px) = sculpt_resize_.start_value;
            sculpt_resize_.active = false;
        } else if (confirm) {
            sculpt_resize_.active = false;
        }
        return;
    }
    if (stroke_.active) {
        if (!in.down[0]) { end_stroke_(); return; }
        if (in.mouse_delta != glm::vec2(0.0f)) continue_stroke_(vp, m);
        return;
    }
    if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt)) begin_stroke_(vp, m, shift, ctrl);
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    viewport_keymap_(ctx, vp, false, false);   // views, shading, sidebar... (no object edits)
    if (ctx.shortcut(Key::F)) sculpt_resize_ = {true, false, m - glm::vec2(sculpt_.radius_px, 0.0f), sculpt_.radius_px};
    if (ctx.shortcut(Key::F, Mods::Shift)) sculpt_resize_ = {true, true, m - glm::vec2(sculpt_.strength * 300.0f, 0.0f), sculpt_.strength};
    // Blender's brush hotkeys.
    if (ctx.shortcut(Key::X)) sculpt_.brush = SculptBrush::Draw;
    if (ctx.shortcut(Key::S)) sculpt_.brush = SculptBrush::Smooth;
    if (ctx.shortcut(Key::I)) sculpt_.brush = SculptBrush::Inflate;
    if (ctx.shortcut(Key::G)) sculpt_.brush = SculptBrush::Grab;
    if (ctx.shortcut(Key::T, Mods::Shift)) sculpt_.brush = SculptBrush::Flatten;
    if (ctx.shortcut(Key::Tab)) exit_mesh_mode_();
}

void EditorApp::draw_sculpt_overlay_(imm::Context& ctx, const ViewProj& vp) {
    if (!sculpt_active_) return;
    draw_brush_overlay_(ctx, vp, sculpt_.radius_px, sculpt_.strength, sculpt_resize_, stroke_.active, stroke_.active && stroke_.invert);
}

void EditorApp::draw_brush_overlay_(imm::Context& ctx, const ViewProj& vp, float radius_px, float strength, const SculptResize& resize,
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
        shadow_text_(ctx, c + glm::vec2(-30, -8), buf, et_.viewport.overlay_text);
        return;
    }
    if (!viewport_hovered_ && !stroking) return;
    glm::vec3 hit, n;
    if (sculpt_hit_(vp, m, hit, n)) {
        const glm::mat4 w = mesh_world_();
        const float r = brush_local_radius_(vp, hit, radius_px);
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

float EditorApp::draw_sculpt_header_(imm::Context& ctx, float x, const imm::Box& hb) {
    using I = imm::Icon;
    const float bh = hb.h - 6;
    auto label = [&](const char* t) {
        const float w = ctx.text_width(t) + 6;
        ctx.text_in({x, hb.y, w, hb.h}, t, ctx.style.text_dim, 0.0f);
        x += w;
    };
    label("Radius");
    ctx.drag_float_box("sc_radius", {x, hb.y + 3, 62, bh}, &sculpt_.radius_px, 0.5f, 2.0f, 600.0f, "%.0f px");
    ctx.tooltip("Radius\nBrush radius in screen pixels (F)");
    x += 68;
    label("Strength");
    ctx.drag_float_box("sc_strength", {x, hb.y + 3, 56, bh}, &sculpt_.strength, 0.005f, 0.0f, 1.0f, "%.2f");
    ctx.tooltip("Strength\nHow strongly each dab acts (Shift F)");
    x += 64;
    return x + 8;   // symmetry sits after the menus (draw_viewport_header_), space permitting
}

std::string EditorApp::symmetry_label_(const MirrorSettings& sym) {
    std::string l = "Mirror";
    if (!sym.any()) return l;
    l += ' ';
    for (int a = 0; a < 3; ++a) if (sym.axis[a]) l += "XYZ"[a];
    if (sym.global) l += " (Global)";
    return l;
}

float EditorApp::draw_symmetry_buttons_(imm::Context& ctx, float x, const imm::Box& hb, MirrorSettings& sym, const char* id,
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

void EditorApp::draw_symmetry_panel_(imm::Context& ctx, MirrorSettings& sym) {
    ctx.checkbox("Mirror X", &sym.axis[0]);
    ctx.checkbox("Mirror Y", &sym.axis[1]);
    ctx.checkbox("Mirror Z", &sym.axis[2]);
    int space = sym.global ? 1 : 0;
    if (ctx.combo("Mirror Axes", &space, {"Local", "Global"})) sym.global = space == 1;
    ctx.tooltip("Mirror Axes\nLocal: the mesh's own axes through its origin. Global: the world's axes through the world origin.");
}

void EditorApp::draw_sculpt_toolbar_(imm::Context& ctx, const imm::Box& r) {
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
        const imm::Box bb{r.x + 3, y, kToolSize, kToolSize};
        y += kToolSize + 2;
        if (ctx.icon_button(std::string("brush_") + sculpt_brush_name(b.b), b.ic, b.tip, sculpt_.brush == b.b, kToolSize,
                            imm::Context::kAll, bb)) {
            sculpt_.brush = b.b;
        }
    }
}

void EditorApp::draw_sculpt_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    if (ctx.collapsing_header("Brush", true, nullptr, I::SculptMode)) {
        int b = static_cast<int>(sculpt_.brush);
        if (ctx.combo("Brush", &b, {"Draw", "Smooth", "Inflate", "Grab", "Flatten"})) sculpt_.brush = static_cast<SculptBrush>(b);
        ctx.drag_float("Radius (px)", &sculpt_.radius_px, 0.5f, 2.0f, 600.0f);
        ctx.slider_float("Strength", &sculpt_.strength, 0.0f, 1.0f, "%.2f");
        ctx.slider_float("Spacing", &sculpt_.spacing, 0.02f, 1.0f, "%.2f");
        draw_symmetry_panel_(ctx, sculpt_.symmetry);
    }
    if (ctx.collapsing_header("Density", true, nullptr, I::Mesh)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%zu vertices, %zu faces", mesh_.mesh.positions.size(), mesh_.mesh.faces.size());
        ctx.label_dim(buf);
        const bool big = mesh_.mesh.faces.size() * 4 > 600000;
        if (ctx.button("Subdivide Smooth", -1, !big, I::Plus)) run_catmull_clark_(1);
        ctx.tooltip("Subdivide Smooth\nCatmull-Clark: every face becomes quads and the surface rounds off");
        if (ctx.button("Subdivide Simple", -1, !big, I::Plus)) {
            MeshSelection all;
            all.select_all(mesh_.mesh);
            mesh_.selection = all;
            run_subdivide_(1);
        }
        ctx.tooltip("Subdivide Simple\nSplit every face without changing the shape");
        if (big) ctx.label_dim("Already dense: subdividing would pass 600k faces.");
        bool smooth = !mesh_.mesh.faces.empty() && mesh_.mesh.faces[0].smooth;
        if (ctx.checkbox("Shade Smooth", &smooth)) {
            mesh_.edit(smooth ? "Shade Smooth" : "Shade Flat", [&](EditMesh& mm, MeshSelection&) {
                for (auto& f : mm.faces) f.smooth = smooth;
            });
        }
        ctx.tooltip("Shade Smooth\nInterpolate normals across faces (the whole mesh)");
    }
}

} // namespace editor
} // namespace toy
