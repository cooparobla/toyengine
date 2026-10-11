#include "editor/app/editor_app.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#include <toyengine/world/heightmap_store.h>
#include <toyengine/world/png16.h>

namespace toy {
namespace editor {

using toy::world::HeightmapData;
using toy::world::HeightmapTerrain;
using toy::world::IRect;

namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief Seconds of holding the mouse still between repeated dabs (raise / smooth keep working). */
constexpr double k_hold_dab_interval = 1.0 / 20.0;
double g_last_dab_time = 0.0;
glm::vec3 g_last_dab_world{0.0f};

struct ToolButton {
    TerrainTool tool;
    imm::Icon icon;
    const char* tip;
};

const ToolButton k_tools[] = {
    {TerrainTool::Raise, imm::Icon::BrushDraw, "Raise / Lower\nBuild the ground up under the brush; Ctrl digs it down (1)"},
    {TerrainTool::Smooth, imm::Icon::BrushSmooth, "Smooth\nSoften bumps and ridges; hold Shift with any tool (2)"},
    {TerrainTool::Flatten, imm::Icon::BrushFlatten, "Flatten\nLevel the ground to the height where the stroke started (3)"},
    {TerrainTool::SetHeight, imm::Icon::Grid, "Set Height\nPull the ground to the Target Height; Ctrl-click picks it from the terrain (4)"},
    {TerrainTool::Noise, imm::Icon::Dots, "Noise\nAdd natural, bumpy variation; Ctrl carves it in (5)"},
    {TerrainTool::Erode, imm::Icon::BrushGrab, "Erode\nWear steep slopes down into gentler ones, as weather would (6)"},
    {TerrainTool::Paint, imm::Icon::VertexPaint, "Paint\nPaint the chosen layer onto the ground; Ctrl erases it (7)"},
};

} // namespace

// =================================================================================
// Lifecycle
// =================================================================================

HeightmapTerrain* EditorApp::terrain_live_() const {
    if (terrain_object_ == 0) return nullptr;
    coopa::scene::SceneObject* live = sync_.live(terrain_object_);
    return live ? live->get_component<HeightmapTerrain>() : nullptr;
}

void EditorApp::enter_terrain_mode_(ObjectId id) {
    if (!is_terrain_object_(id) || playing()) {
        log_warn("Terrain Mode: select an object with a HeightmapTerrain component first");
        return;
    }
    if (interaction_mode() != InteractionMode::Object) set_interaction_mode(InteractionMode::Object);
    terrain_mode_ = true;
    terrain_object_ = id;
    terrain_staged_ = TerrainStaged{};
    tool_ = Tool::Select;
    prop_tab_ = PropTab::Tool;
    doc_.clear_selection();
    doc_.select(id, true);
}

void EditorApp::exit_terrain_mode_() {
    if (terrain_stroke_.active) terrain_end_stroke_();
    terrain_resize_.active = false;
    terrain_mode_ = false;
    terrain_object_ = 0;
}

void EditorApp::terrain_frame_() {
    if (!terrain_mode_) return;
    if (playing() || !doc_.find(terrain_object_) || !is_terrain_object_(terrain_object_)) exit_terrain_mode_();
}

// =================================================================================
// Strokes
// =================================================================================

bool EditorApp::terrain_hit_(const ViewProj& vp, glm::vec2 px, glm::vec3& world_hit) const {
    const HeightmapTerrain* t = terrain_live_();
    if (t == nullptr || t->data == nullptr) return false;
    glm::vec3 o, d;
    vp.ray(px, o, d);
    const glm::vec3 origin = t->origin();
    float hit_t = 0.0f;
    if (!t->data->raycast(o - origin, d, 1e5f, hit_t)) return false;
    world_hit = o + d * hit_t;
    return true;
}

void EditorApp::terrain_dab_at_(const ViewProj& vp, glm::vec2 px) {
    glm::vec3 hit;
    if (!terrain_hit_(vp, px, hit) || !terrain_stroke_.data) return;
    const HeightmapTerrain* t = terrain_live_();
    if (t == nullptr) return;
    TerrainBrushSettings s = terrain_brush_;
    s.tool = terrain_stroke_.tool;
    const glm::vec2 local = glm::vec2(hit) - glm::vec2(t->origin());
    const IRect r = terrain_dab(*terrain_stroke_.data, s, local, terrain_stroke_.invert, terrain_stroke_.flatten_to,
                                terrain_stroke_.seed);
    if (s.tool == TerrainTool::Paint) terrain_stroke_.splat_rect.include(r);
    else terrain_stroke_.height_rect.include(r);
    g_last_dab_time = now_seconds();
    g_last_dab_world = hit;
}

void EditorApp::terrain_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl) {
    HeightmapTerrain* t = terrain_live_();
    glm::vec3 hit;
    if (t == nullptr || !terrain_hit_(vp, px, hit)) return;
    // Set Height: Ctrl-click samples the terrain instead of painting.
    if (terrain_brush_.tool == TerrainTool::SetHeight && ctrl) {
        terrain_brush_.target_height = t->data->sample_height(glm::vec2(hit) - glm::vec2(t->origin()));
        log_info("Target Height set to " + std::to_string(static_cast<int>(std::lround(terrain_brush_.target_height))) + " m");
        return;
    }
    TerrainStroke& s = terrain_stroke_;
    s = TerrainStroke{};
    s.active = true;
    s.invert = ctrl;
    s.tool = shift ? TerrainTool::Smooth : terrain_brush_.tool;
    s.data = t->data;
    s.flatten_to = t->data->sample_height(glm::vec2(hit) - glm::vec2(t->origin()));
    s.seed = static_cast<std::uint32_t>(std::random_device{}());
    s.last_px = px;
    if (s.tool == TerrainTool::Paint) {
        s.start_splat0 = s.data->splat(0);
        s.start_splat1 = s.data->splat(1);
    } else {
        s.start_heights = s.data->heights();
    }
    terrain_dab_at_(vp, px);
}

void EditorApp::terrain_continue_stroke_(const ViewProj& vp, glm::vec2 px) {
    TerrainStroke& s = terrain_stroke_;
    // Dabs along the cursor's path, spaced by a fraction of the brush (in metres on the ground).
    const glm::vec2 from = s.last_px;
    const float len = glm::distance(px, from);
    const int steps = std::max(1, static_cast<int>(len / 3.0f));
    const float spacing = std::max(0.05f, terrain_brush_.spacing * terrain_brush_.radius);
    for (int k = 1; k <= steps; ++k) {
        const glm::vec2 p = from + (px - from) * (static_cast<float>(k) / static_cast<float>(steps));
        glm::vec3 hit;
        if (!terrain_hit_(vp, p, hit)) continue;
        if (glm::distance(glm::vec2(hit), glm::vec2(g_last_dab_world)) >= spacing) terrain_dab_at_(vp, p);
    }
    s.last_px = px;
}

void EditorApp::terrain_end_stroke_() {
    TerrainStroke& s = terrain_stroke_;
    if (!s.active) return;
    s.active = false;
    if (!s.data || (s.height_rect.empty() && s.splat_rect.empty())) return;
    TerrainPatch before, after;
    if (s.tool == TerrainTool::Paint) {
        before = capture_patch_from(s.data, s.data->heights(), s.start_splat0, s.start_splat1, IRect{}, s.splat_rect);
        after = capture_patch(s.data, IRect{}, s.splat_rect);
    } else {
        before = capture_patch_from(s.data, s.start_heights, s.data->splat(0), s.data->splat(1), s.height_rect, IRect{});
        after = capture_patch(s.data, s.height_rect, IRect{});
    }
    terrain_undo_.set_budget(256u << 20, [](const TerrainPatch& p) { return p.bytes(); }, 4);
    terrain_undo_.push(std::string("Terrain ") + terrain_tool_name(s.tool), std::move(before), std::move(after));
    s = TerrainStroke{};
}

void EditorApp::terrain_push_whole_(const std::string& label, const std::shared_ptr<HeightmapData>& data,
                                    const std::vector<float>& heights, const std::vector<std::uint8_t>& s0,
                                    const std::vector<std::uint8_t>& s1) {
    const IRect all_h{0, 0, data->nx(), data->ny()}, all_s{0, 0, data->splat_w(), data->splat_h()};
    TerrainPatch before = capture_patch_from(data, heights, s0, s1, all_h, all_s);
    TerrainPatch after = capture_patch(data, all_h, all_s);
    terrain_undo_.set_budget(256u << 20, [](const TerrainPatch& p) { return p.bytes(); }, 4);
    terrain_undo_.push(label, std::move(before), std::move(after));
}

void EditorApp::terrain_apply_undo_(bool redo) {
    const TerrainPatch* p = redo ? terrain_undo_.redo() : terrain_undo_.undo();
    if (p != nullptr && !apply_patch(*p)) log_warn("Terrain undo skipped: the terrain was resized or removed since");
}

// =================================================================================
// Input / overlay
// =================================================================================

void EditorApp::handle_terrain_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool shift = has(in.mods, Mods::Shift);
    const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
    if (terrain_resize_.active) {
        const float dx = m.x - terrain_resize_.start.x;
        if (terrain_resize_.strength) terrain_brush_.strength = std::clamp(terrain_resize_.start_value + dx / 300.0f, 0.0f, 1.0f);
        else terrain_brush_.radius = std::clamp(terrain_resize_.start_value * std::exp(dx / 200.0f), 0.5f, 500.0f);
        bool cancel = in.pressed[1], confirm = in.pressed[0];
        for (const auto& e : in.keys) {
            if (e.action == coopa::input::KeyAction::Release) continue;
            if (e.key == Key::Escape) cancel = true;
            if (e.key == Key::Enter || e.key == Key::KpEnter) confirm = true;
        }
        ctx.consume_keyboard();
        if (cancel) {
            (terrain_resize_.strength ? terrain_brush_.strength : terrain_brush_.radius) = terrain_resize_.start_value;
            terrain_resize_.active = false;
        } else if (confirm) {
            terrain_resize_.active = false;
        }
        return;
    }
    if (terrain_stroke_.active) {
        if (!in.down[0]) { terrain_end_stroke_(); return; }
        if (in.mouse_delta != glm::vec2(0.0f)) terrain_continue_stroke_(vp, m);
        else if (terrain_stroke_.tool != TerrainTool::Paint && now_seconds() - g_last_dab_time > k_hold_dab_interval) {
            terrain_dab_at_(vp, m);   // holding still keeps raising / smoothing
        }
        return;
    }
    if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt)) terrain_begin_stroke_(vp, m, shift, ctrl);
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    viewport_keymap_(ctx, vp, false, false);
    if (ctx.shortcut(Key::F)) terrain_resize_ = {true, false, m, terrain_brush_.radius};
    if (ctx.shortcut(Key::F, Mods::Shift)) terrain_resize_ = {true, true, m - glm::vec2(terrain_brush_.strength * 300.0f, 0.0f), terrain_brush_.strength};
    if (ctx.shortcut(Key::LeftBracket)) terrain_brush_.radius = std::max(0.5f, terrain_brush_.radius / 1.15f);
    if (ctx.shortcut(Key::RightBracket)) terrain_brush_.radius = std::min(500.0f, terrain_brush_.radius * 1.15f);
    const Key digits[] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7};
    for (int i = 0; i < 7; ++i) {
        if (ctx.shortcut(digits[i])) terrain_brush_.tool = static_cast<TerrainTool>(i);
    }
    if (ctx.shortcut(Key::Tab) || ctx.shortcut(Key::Escape)) exit_terrain_mode_();
}

void EditorApp::draw_terrain_overlay_(imm::Context& ctx, const ViewProj& vp) {
    const HeightmapTerrain* t = terrain_live_();
    if (t == nullptr || t->data == nullptr) return;
    const glm::vec4 col = terrain_stroke_.invert ? glm::vec4(0.55f, 0.75f, 1.0f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
    glm::vec3 centre;
    const glm::vec2 m = ctx.mouse();
    if (terrain_resize_.active) {
        if (!terrain_hit_(vp, terrain_resize_.start, centre)) return;
    } else if (!(viewport_hovered_ || terrain_stroke_.active) || !terrain_hit_(vp, m, centre)) {
        return;
    }
    const glm::vec3 origin = t->origin();
    // A ring draped over the ground: each point at the terrain's height there.
    auto ring = [&](float radius, const glm::vec4& c, float width) {
        std::optional<glm::vec2> prev;
        const int n = 72;
        for (int i = 0; i <= n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            const glm::vec2 xy = glm::vec2(centre) + glm::vec2(std::cos(a), std::sin(a)) * radius;
            const glm::vec3 p(xy, origin.z + t->data->sample_height(xy - glm::vec2(origin)) + 0.05f);
            auto q = vp.project(p);
            if (prev && q) ctx.line(*prev, *q, c, width);
            prev = q;
        }
    };
    const float r = terrain_brush_.radius;
    ring(r, col, 1.6f);
    // The fully weighted core (falloff), dimmer.
    ring(r * std::max(0.02f, 1.0f - terrain_brush_.falloff), imm::with_alpha(col, 0.45f), 1.0f);
    if (terrain_resize_.active) {
        char buf[64];
        if (terrain_resize_.strength) std::snprintf(buf, sizeof(buf), "Strength %.2f", terrain_brush_.strength);
        else std::snprintf(buf, sizeof(buf), "Radius %.1f m", terrain_brush_.radius);
        if (auto q = vp.project(centre)) shadow_text_(ctx, *q + glm::vec2(-30, -8), buf, et_.viewport.overlay_text);
    }
}

// =================================================================================
// UI
// =================================================================================

float EditorApp::draw_terrain_header_(imm::Context& ctx, float x, const imm::Box& hb) {
    const float bh = hb.h - 6;
    auto label = [&](const char* t) {
        const float w = ctx.text_width(t) + 6;
        ctx.text_in({x, hb.y, w, hb.h}, t, ctx.style.text_dim, 0.0f);
        x += w;
    };
    label(terrain_tool_name(terrain_brush_.tool));
    x += 6;
    label("Radius");
    ctx.drag_float_box("tr_radius", {x, hb.y + 3, 66, bh}, &terrain_brush_.radius, 0.1f, 0.5f, 500.0f, "%.1f m");
    ctx.tooltip("Radius\nBrush radius in metres on the ground (F, [ and ])");
    x += 72;
    label("Strength");
    ctx.drag_float_box("tr_strength", {x, hb.y + 3, 56, bh}, &terrain_brush_.strength, 0.005f, 0.0f, 1.0f, "%.2f");
    ctx.tooltip("Strength\nHow strongly each dab acts (Shift F)");
    x += 64;
    return x + 8;
}

void EditorApp::draw_terrain_toolbar_(imm::Context& ctx, const imm::Box& r) {
    float y = r.y + 4;
    for (const auto& b : k_tools) {
        const imm::Box bb{r.x + 3, y, kToolSize, kToolSize};
        y += kToolSize + 2;
        if (ctx.icon_button(std::string("terrain_") + terrain_tool_name(b.tool), b.icon, b.tip, terrain_brush_.tool == b.tool,
                            kToolSize, imm::Context::kAll, bb)) {
            terrain_brush_.tool = b.tool;
        }
    }
}

void EditorApp::terrain_edit_component_(const std::string& label, const std::function<void(Node&)>& fn, const std::string& merge) {
    Node comp;
    if (!shown_component_(terrain_object_, "HeightmapTerrain", comp)) return;
    fn(comp);
    edit_component_(terrain_object_, comp, label, merge);
}

void EditorApp::draw_terrain_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    HeightmapTerrain* t = terrain_live_();
    const Node* obj = doc_.find(terrain_object_);
    ctx.label(std::string("Terrain: ") + (obj ? get_string(*obj, "name", "?") : "?"));
    if (ctx.button("Exit Terrain Mode", -1, true, I::ObjectMode)) { exit_terrain_mode_(); return; }
    ctx.tooltip("Exit Terrain Mode (Tab)");
    if (t == nullptr || t->data == nullptr) {
        ctx.label_dim("The terrain is still loading.");
        return;
    }

    if (ctx.collapsing_header("Tools", true, nullptr, I::Terrain)) {
        for (const auto& b : k_tools) {
            if (ctx.selectable(terrain_tool_name(b.tool), terrain_brush_.tool == b.tool, 0.0f, b.icon)) terrain_brush_.tool = b.tool;
            ctx.tooltip(b.tip);
        }
    }

    if (ctx.collapsing_header("Brush", true, nullptr, I::BrushDraw)) {
        ctx.drag_float("Radius (m)", &terrain_brush_.radius, 0.1f, 0.5f, 500.0f);
        ctx.tooltip("Radius\nIn metres on the ground. F drags it; [ and ] step it.");
        ctx.slider_float("Strength", &terrain_brush_.strength, 0.0f, 1.0f, "%.2f");
        ctx.slider_float("Falloff", &terrain_brush_.falloff, 0.0f, 1.0f, "%.2f");
        ctx.tooltip("Falloff\n0: a hard-edged disc. 1: soft from the centre out. The inner ring shows the fully weighted core.");
        ctx.slider_float("Spacing", &terrain_brush_.spacing, 0.05f, 1.0f, "%.2f");
        ctx.tooltip("Spacing\nDistance between dabs along a stroke, as a fraction of the radius");
        if (terrain_brush_.tool == TerrainTool::SetHeight) {
            ctx.drag_float("Target Height (m)", &terrain_brush_.target_height, 0.1f, 0.0f, t->data->geometry().height);
            ctx.label_dim("Ctrl-click the terrain to pick a height.");
        }
        if (terrain_brush_.tool == TerrainTool::Noise) {
            ctx.drag_float("Noise Scale (m)", &terrain_brush_.noise_scale, 0.1f, 1.0f, 500.0f);
        }
        if (terrain_brush_.tool == TerrainTool::Paint) {
            std::vector<std::string> names;
            for (const auto& l : t->layers) names.push_back(l.name);
            int layer = std::clamp(terrain_brush_.layer, 0, std::max(0, static_cast<int>(names.size()) - 1));
            if (!names.empty() && ctx.combo("Layer", &layer, names)) terrain_brush_.layer = layer;
            terrain_brush_.layer = layer;
            ctx.label_dim("Ctrl paints the layer away.");
        }
    }

    draw_terrain_layers_(ctx, *t);
    draw_terrain_settings_(ctx, *t);
    draw_terrain_heightmap_(ctx, *t);
    draw_terrain_details_(ctx);
}

void EditorApp::draw_terrain_layers_(imm::Context& ctx, HeightmapTerrain& t) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Layers", true, nullptr, I::Palette)) return;
    Node comp;
    if (!shown_component_(terrain_object_, "HeightmapTerrain", comp)) return;
    const bool has_layers = comp.contains("layers") && comp.at("layers").is_sequence();
    const int count = has_layers ? static_cast<int>(comp.at("layers").as_seq().size()) : 0;

    std::vector<std::string> textures{""};
    for (const auto& p : inspector_env_().list_assets("textures", ".png")) textures.push_back(p);
    std::vector<std::string> shown = textures;
    shown[0] = "(none)";

    for (int i = 0; i < count; ++i) {
        const Node& ln = comp.at("layers").as_seq()[static_cast<std::size_t>(i)];
        const std::string name = get_string(ln, "name", "layer" + std::to_string(i));
        const std::string key = "tlayer" + std::to_string(i);
        ctx.push_id(key);
        if (ctx.collapsing_header(std::to_string(i) + "  " + name, false, nullptr,
                                  terrain_brush_.tool == TerrainTool::Paint && terrain_brush_.layer == i ? I::VertexPaint : I::Texture)) {
            auto edit = [&](const std::string& label, const std::function<void(Node&)>& fn, const std::string& merge = {}) {
                terrain_edit_component_(label, [&](Node& c) { fn(c["layers"].as_seq()[static_cast<std::size_t>(i)]); }, merge);
            };
            if (ctx.button("Paint This Layer", -1, true, I::VertexPaint)) {
                terrain_brush_.tool = TerrainTool::Paint;
                terrain_brush_.layer = i;
            }
            std::string n = name;
            if (ctx.input_text("Name", &n)) edit("Rename Terrain Layer", [&](Node& l) { l["name"] = n; });
            auto texture_combo = [&](const char* label, const char* field) {
                const std::string cur = get_string(ln, field, "");
                int idx = 0;
                for (std::size_t k = 0; k < textures.size(); ++k) if (textures[k] == cur) idx = static_cast<int>(k);
                if (idx == 0 && !cur.empty()) { textures.push_back(cur); shown.push_back(cur); idx = static_cast<int>(textures.size()) - 1; }
                if (ctx.combo(label, &idx, shown)) {
                    const std::string v = textures[static_cast<std::size_t>(idx)];
                    edit(std::string("Terrain Layer ") + label, [&](Node& l) {
                        if (v.empty()) l.as_map().erase(field);
                        else l[field] = v;
                    });
                }
            };
            texture_combo("Albedo", "albedo");
            texture_combo("Normal Map", "normal");
            auto f = [&](const char* label, const char* field, float def, float speed, float lo, float hi, bool slider) {
                float v = ln.contains(field) ? ln.at(field).get_value<float>() : def;
                const bool ch = slider ? ctx.slider_float(label, &v, lo, hi, "%.2f") : ctx.drag_float(label, &v, speed, lo, hi);
                if (ch) edit(std::string("Terrain Layer ") + label, [&](Node& l) { l[field] = v; }, key + field);
            };
            float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            if (ln.contains("tint")) {
                tint[0] = ln.at("tint").contains("r") ? ln.at("tint").at("r").get_value<float>() : 1.0f;
                tint[1] = ln.at("tint").contains("g") ? ln.at("tint").at("g").get_value<float>() : 1.0f;
                tint[2] = ln.at("tint").contains("b") ? ln.at("tint").at("b").get_value<float>() : 1.0f;
            }
            if (ctx.color_edit("Tint", tint)) {
                edit("Terrain Layer Tint", [&](Node& l) {
                    Node c = Node::mapping();
                    c["r"] = tint[0];
                    c["g"] = tint[1];
                    c["b"] = tint[2];
                    l["tint"] = c;
                }, key + "tint");
            }
            f("Tiling (m)", "tiling", 8.0f, 0.05f, 0.1f, 500.0f, false);
            ctx.tooltip("Tiling\nMetres one repeat of the texture covers");
            f("Roughness", "roughness", 0.85f, 0.0f, 0.0f, 1.0f, true);
            f("Metallic", "metallic", 0.0f, 0.0f, 0.0f, 1.0f, true);
            f("Normal Strength", "normal_strength", 1.0f, 0.0f, 0.0f, 2.0f, true);
            bool tri = ln.contains("triplanar") && ln.at("triplanar").get_value<bool>();
            if (ctx.checkbox("Triplanar (cliffs)", &tri)) edit("Terrain Layer Triplanar", [&](Node& l) { l["triplanar"] = tri; });
            ctx.tooltip("Triplanar\nOn steep slopes, project the texture from the sides as well as from above, so cliffs are not smeared");
            if (tri) f("Triplanar Sharpness", "triplanar_sharpness", 4.0f, 0.05f, 1.0f, 16.0f, false);
            if (ctx.button("Remove Layer", -1, count > 1, I::Trash)) {
                terrain_edit_component_("Remove Terrain Layer", [&](Node& c) {
                    auto& seq = c["layers"].as_seq();
                    seq.erase(seq.begin() + i);
                });
                if (terrain_brush_.layer >= count - 1) terrain_brush_.layer = std::max(0, count - 2);
                ctx.pop_id();
                return;
            }
        }
        ctx.pop_id();
    }
    if (ctx.button("Add Layer", -1, count < toy::world::k_heightmap_layer_count, I::Plus)) {
        terrain_edit_component_("Add Terrain Layer", [&](Node& c) {
            if (!c.contains("layers") || !c.at("layers").is_sequence()) c["layers"] = Node::sequence();
            Node l = Node::mapping();
            l["name"] = std::string("layer") + std::to_string(count);
            l["tiling"] = 8.0f;
            c["layers"].as_seq().push_back(l);
        });
    }
    ctx.tooltip(count < toy::world::k_heightmap_layer_count ? "Add Layer\nA new paint layer (up to 8)" : "A terrain has at most 8 layers");
    (void)t;
}

void EditorApp::draw_terrain_settings_(imm::Context& ctx, HeightmapTerrain& t) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Terrain Size", false, nullptr, I::Scale)) return;
    const toy::world::HeightmapGeometry& g = t.data->geometry();
    TerrainStaged& s = terrain_staged_;
    if (s.id != terrain_object_) {
        s.id = terrain_object_;
        s.size = g.size;
        s.height = g.height;
        s.chunk = g.chunk_size;
        s.resolution = g.resolution;
        s.splat_resolution = g.splat_resolution;
    }
    ctx.drag_float("Width (m)", &s.size.x, 1.0f, 8.0f, 32768.0f);
    ctx.drag_float("Depth (m)", &s.size.y, 1.0f, 8.0f, 32768.0f);
    ctx.drag_float("Height (m)", &s.height, 0.5f, 1.0f, 8000.0f);
    ctx.tooltip("Height\nMetres at the heightmap's full value; the shape is kept (scaled) when this changes");
    ctx.drag_float("Chunk Size (m)", &s.chunk, 1.0f, 8.0f, 1024.0f);
    ctx.tooltip("Chunk Size\nThe unit the terrain is drawn, culled, collided and scattered in");
    ctx.drag_int("Heightmap Resolution", &s.resolution, 1.0f, 33, 8193);
    ctx.tooltip("Heightmap Resolution\nHeight samples per side (2^n + 1 is ideal: 513, 1025, 2049). The heights are resampled.");
    ctx.drag_int("Paint Resolution", &s.splat_resolution, 1.0f, 16, 8192);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%.2f m per height sample, %d x %d chunks", s.size.x / static_cast<float>(std::max(2, s.resolution) - 1),
                  static_cast<int>(std::ceil(s.size.x / std::max(1.0f, s.chunk))), static_cast<int>(std::ceil(s.size.y / std::max(1.0f, s.chunk))));
    ctx.label_dim(buf);
    const bool changed = s.size != g.size || s.height != g.height || s.chunk != g.chunk_size || s.resolution != g.resolution ||
                         s.splat_resolution != g.splat_resolution;
    if (ctx.button("Apply", -1, changed, I::Check)) {
        terrain_edit_component_("Resize Terrain", [&](Node& c) {
            Node size = Node::mapping();
            size["x"] = s.size.x;
            size["y"] = s.size.y;
            c["size"] = size;
            c["height"] = s.height;
            c["chunk_size"] = s.chunk;
            c["resolution"] = s.resolution;
            c["splat_resolution"] = s.splat_resolution;
        });
        s.id = 0;   // re-read from the resized data next frame
    }
    if (changed && ctx.button("Revert", -1, true, I::Undo)) s.id = 0;
}

void EditorApp::draw_terrain_heightmap_(imm::Context& ctx, HeightmapTerrain& t) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Heightmap", false, nullptr, I::Image)) return;
    const std::shared_ptr<HeightmapData> data = t.data;
    const std::string file = project_.relative(t.files.height);
    ctx.label_dim(file.empty() ? t.files.height : file);
    if (data->dirty_on_disk()) ctx.label_dim("Unsaved edits: written with the scene (Ctrl S).");
    if (ctx.button("Import Heightmap...", -1, true, I::Folder)) {
        const char* home = std::getenv("HOME");
        file_dialog_.open(ui(), FileDialog::Mode::OpenFile, "Import Heightmap", home ? fs::path(home) : project_.root(), {".png"},
                          [this, data](const fs::path& p) {
                              deferred_.push_back([this, data, p] {
                                  const std::vector<float> h = data->heights();
                                  const auto s0 = data->splat(0), s1 = data->splat(1);
                                  if (!toy::world::load_heightmap_image(*data, p.string())) {
                                      log_error("Could not read " + p.string());
                                      return;
                                  }
                                  terrain_push_whole_("Import Heightmap", data, h, s0, s1);
                                  log_info("Imported " + p.filename().string() + " (resampled to the terrain's resolution)");
                              });
                          });
    }
    ctx.tooltip("Import Heightmap\nReplace the terrain's shape with a greyscale PNG (16-bit for smooth slopes; 8-bit works). "
                "White is the terrain's full Height. Undoable.");
    if (ctx.button("Export Heightmap...", -1, true, I::Save)) {
        file_dialog_.open(ui(), FileDialog::Mode::SaveFile, "Export Heightmap", project_.assets(), {".png"},
                          [this, data](const fs::path& p) {
                              fs::path out = p;
                              if (out.extension() != ".png") out += ".png";
                              if (toy::world::save_heightmap_image(*data, out.string())) log_info("Exported " + out.filename().string());
                              else log_error("Could not write " + out.string());
                          });
    }
    ctx.tooltip("Export Heightmap\nSave the current shape as a 16-bit greyscale PNG");

    static toy::world::HeightmapGenerate gen;
    int seed = static_cast<int>(gen.seed);
    if (ctx.drag_int("Seed", &seed, 0.2f, 0, 1 << 30)) gen.seed = static_cast<std::uint32_t>(seed);
    ctx.drag_float("Feature Size (m)", &gen.feature_size, 1.0f, 8.0f, 4096.0f);
    ctx.slider_float("Ridges", &gen.ridges, 0.0f, 1.0f, "%.2f");
    ctx.slider_float("Base", &gen.base, 0.0f, 1.0f, "%.2f");
    ctx.slider_float("Amplitude", &gen.amplitude, 0.0f, 1.0f, "%.2f");
    ctx.checkbox("Island", &gen.island);
    if (ctx.button("Generate", -1, true, I::Restart)) {
        const std::vector<float> h = data->heights();
        const auto s0 = data->splat(0), s1 = data->splat(1);
        data->generate(gen);
        terrain_push_whole_("Generate Terrain", data, h, s0, s1);
    }
    ctx.tooltip("Generate\nReplace the shape with procedural hills and ridges. Undoable.");
    if (ctx.button("Fill to Target Height", -1, true, I::Grid)) {
        const std::vector<float> h = data->heights();
        const auto s0 = data->splat(0), s1 = data->splat(1);
        const float v = std::clamp(terrain_brush_.target_height, 0.0f, data->geometry().height);
        std::fill(data->heights().begin(), data->heights().end(), v);
        data->mark_all_dirty();
        terrain_push_whole_("Fill Terrain", data, h, s0, s1);
    }
    ctx.tooltip("Fill to Target Height\nFlatten the whole terrain to the Brush's Target Height -- a blank start. Undoable.");
}

void EditorApp::draw_terrain_details_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Details (Foliage)", false, nullptr, I::Sphere)) return;
    ctx.label_dim("Grass, trees and rocks scattered over the terrain (ParticleSystem, shape: terrain).");
    const Node* obj = doc_.find(terrain_object_);
    int n = 0;
    std::vector<ObjectId> kids;
    if (obj != nullptr && obj->contains("children") && obj->at("children").is_sequence()) {
        for (const auto& k : obj->at("children").as_seq()) kids.push_back(SceneDocument::id_of(k));
    }
    for (ObjectId child : kids) {
        const Node* c = doc_.find(child);
        if (c == nullptr || doc_.find_component(child, "ParticleSystem") < 0) continue;
        ++n;
        if (ctx.selectable(get_string(*c, "name", "?"), false, 0.0f, I::Object)) {
            exit_terrain_mode_();
            doc_.clear_selection();
            doc_.select(child, true);
            prop_tab_ = PropTab::Components;
            return;
        }
        ctx.tooltip("Select it to edit its density, layer, slope limits and mesh (Components tab)");
    }
    if (n == 0) ctx.label_dim("None yet.");
    if (ctx.button("Add Grass", -1, true, I::Plus)) terrain_add_scatter_("grass");
    if (ctx.button("Add Trees", -1, true, I::Plus)) terrain_add_scatter_("tree");
    if (ctx.button("Add Rocks", -1, true, I::Plus)) terrain_add_scatter_("rock");
}

void EditorApp::terrain_add_scatter_(const std::string& kind) {
    const HeightmapTerrain* t = terrain_live_();
    const std::string first_layer = t && !t->layers.empty() ? t->layers.front().name : std::string();
    Node obj = doc_.make_object(doc_.unique_name(kind == "tree" ? "trees" : kind == "rock" ? "rocks" : "grass", terrain_object_));
    std::string yaml;
    if (kind == "grass") {
        yaml = "type: ParticleSystem\nmode: scatter\nshape: terrain\nterrain: { layer: " + first_layer +
               ", density: 1.0, min_weight: 0.35, slope_max: 30, sink: 0.05 }\nseed: 11\nalign_to_normal: true\nrandom_spin: true\n"
               "start_size: [0.6, 1.1]\nmax_draw_distance: 70\nrender_mode: mesh\nrender_mesh: nature/grass_tuft\n"
               "material: { albedo: { r: 0.85, g: 1.0, b: 0.75 }, roughness: 0.8, alpha_mode: CUTOUT, alpha_cutoff: 0.4, "
               "texture_albedo: textures/terrain/layers/grass_blades.png, texture_alpha_mask: textures/terrain/layers/grass_blades.png }\n";
    } else if (kind == "tree") {
        yaml = "type: ParticleSystem\nmode: scatter\nshape: terrain\nterrain: { density: 0.01, slope_max: 24, sink: 0.3 }\nseed: 77\n"
               "random_spin: true\nstart_size: [0.8, 1.3]\nmax_draw_distance: 600\nrender_mode: mesh\nrender_mesh: nature/pine_canopy\n"
               "material: { albedo: { r: 0.16, g: 0.3, b: 0.14 }, roughness: 0.85 }\n";
    } else {
        yaml = "type: ParticleSystem\nmode: scatter\nshape: terrain\nterrain: { density: 0.005, min_weight: 0.3, sink: 0.25 }\nseed: 5\n"
               "align_to_normal: true\nrandom_spin: true\nstart_size: [0.8, 2.6]\nmax_draw_distance: 350\nrender_mode: mesh\n"
               "render_mesh: nature/boulder\nmaterial: { albedo: { r: 0.48, g: 0.46, b: 0.43 }, roughness: 0.8 }\n";
    }
    obj["components"].as_seq().push_back(Node::deserialize(yaml));
    const ObjectId id = doc_.add_object(obj, terrain_object_, -1, "Add Terrain " + kind);
    after_structure_change_(id);
    // Stay in Terrain Mode with the terrain selected.
    doc_.clear_selection();
    doc_.select(terrain_object_, true);
}

void EditorApp::save_terrains_() {
    const int n = toy::world::HeightmapStore::instance().save_dirty();
    if (n > 0) log_info("Saved " + std::to_string(n) + (n == 1 ? " terrain" : " terrains"));
}

} // namespace editor
} // namespace toy
