#include "editor/app/editor_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <coopa/maps/biome.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_map_data.h>

namespace toy {
namespace editor {

using toy::world::IRect;
using toy::world::TerrainComponent;
using toy::world::TileKind;
using toy::world::TileMapData;
using toy::world::TileRecord;

namespace {

double tile_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* const k_kind_names[toy::world::k_tile_kind_count] = {
    "Grass", "Dirt", "Stone", "Sand", "Snow", "Rock", "Water", "Ice", "Moss", "Clay", "Ash", "Salt"};

std::vector<std::string> kind_options() {
    return std::vector<std::string>(std::begin(k_kind_names), std::end(k_kind_names));
}

std::vector<std::string> biome_options() {
    std::vector<std::string> out;
    for (std::size_t b = 0; b < coopa::maps::k_biome_count; ++b) {
        std::string n(coopa::maps::biome_name(static_cast<coopa::maps::Biome>(b)));
        out.push_back(n);
    }
    return out;
}

struct ToolButton {
    TileTool tool;
    imm::Icon icon;
    const char* tip;
};

const ToolButton k_tile_tools[] = {
    {TileTool::Select, imm::Icon::SelectBox, "Select\nDrag a rectangle of tiles (Shift adds, Ctrl removes); double-click selects the "
                                             "connected tiles alike; edit them in Properties (1)"},
    {TileTool::Raise, imm::Icon::BrushDraw, "Raise / Lower\nOne step up per tile the brush passes over; Ctrl lowers (2)"},
    {TileTool::Flatten, imm::Icon::BrushFlatten, "Flatten\nLevel the brush area to the height of the tile you start on (3)"},
    {TileTool::SetHeight, imm::Icon::Grid, "Set Height\nSet tiles to the Target Height; Ctrl-click picks it from a tile (4)"},
    {TileTool::Paint, imm::Icon::VertexPaint, "Paint\nPaint the chosen surface onto tile tops (5)"},
    {TileTool::Smooth, imm::Icon::BrushSmooth, "Smooth\nEase steep, jagged steps toward their neighbours, a step at a time (6)"},
    {TileTool::Water, imm::Icon::Plane, "Water\nFlood tiles below the Water Level; Ctrl drains water back to its bed (7)"},
};

} // namespace

// =================================================================================
// Lifecycle
// =================================================================================

TerrainComponent* EditorApp::tile_live_() const {
    if (tile_object_ == 0) return nullptr;
    coopa::scene::SceneObject* live = sync_.live(tile_object_);
    return live ? live->get_component<TerrainComponent>() : nullptr;
}

void EditorApp::enter_tile_mode_(ObjectId id) {
    if (!is_tile_terrain_object_(id) || playing()) {
        log_warn("Tile Mode: select an object with a TileTerrain component first");
        return;
    }
    if (in_terrain_mode_()) exit_terrain_mode_();
    if (interaction_mode() != InteractionMode::Object) set_interaction_mode(InteractionMode::Object);
    tile_mode_ = true;
    tile_object_ = id;
    tool_ = Tool::Select;
    prop_tab_ = PropTab::Tool;
    tile_sel_.reset(0, 0);
    doc_.clear_selection();
    doc_.select(id, true);
}

void EditorApp::exit_tile_mode_() {
    if (tile_stroke_.active) tile_end_stroke_();
    tile_stroke_ = TileStroke{};
    tile_mode_ = false;
    tile_object_ = 0;
    tile_hover_valid_ = false;
}

void EditorApp::tile_frame_() {
    // A Regenerate lands a few frames after it was asked for: record its undo step then.
    if (tile_regen_data_ && tile_regen_data_->bakes_done() > tile_regen_wait_) {
        auto data = tile_regen_data_;
        tile_regen_data_.reset();
        if (data->width() == tile_regen_w_ && data->height() == tile_regen_h_) {
            tile_push_undo_("Regenerate World", data, tile_regen_before_, IRect{0, 0, data->width(), data->height()});
        } else {
            log_warn("Regenerate World changed the world's size: it cannot be undone");
        }
        tile_regen_before_.clear();
        log_info("World regenerated");
    }
    if (!tile_mode_) return;
    if (playing() || !doc_.find(tile_object_) || !is_tile_terrain_object_(tile_object_)) exit_tile_mode_();
}

// =================================================================================
// Strokes
// =================================================================================

bool EditorApp::tile_hit_(const ViewProj& vp, glm::vec2 px, glm::ivec2& tile, glm::vec3* world) const {
    const TerrainComponent* t = tile_live_();
    if (t == nullptr || !t->tiles || t->tiles->empty() || t->owner == nullptr) return false;
    glm::vec3 o, d;
    vp.ray(px, o, d);
    const glm::mat4 m = t->owner->get_transform() ? t->owner->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
    const glm::mat4 inv = glm::inverse(m);
    const glm::vec3 lo = glm::vec3(inv * glm::vec4(o, 1.0f));
    const glm::vec3 ld = glm::normalize(glm::vec3(inv * glm::vec4(d, 0.0f)));
    toy::world::TileHit hit;
    if (!t->tiles->raycast(lo, ld, t->params.tile_size, t->params.height_step, 1e5f, hit)) return false;
    tile = hit.tile;
    if (world) *world = glm::vec3(m * glm::vec4(hit.point, 1.0f));
    return true;
}

glm::vec3 EditorApp::tile_corner_(const TerrainComponent& t, int x, int y, float steps) const {
    const glm::mat4 m = t.owner && t.owner->get_transform() ? t.owner->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
    return glm::vec3(m * glm::vec4(static_cast<float>(x) * t.params.tile_size, static_cast<float>(y) * t.params.tile_size,
                                   steps * t.params.height_step, 1.0f));
}

void EditorApp::tile_dab_(const glm::ivec2& tile) {
    TileStroke& s = tile_stroke_;
    if (!s.data) return;
    TileMapData& d = *s.data;
    const IRect brush = tile_brush_rect(tile, tile_brush_.size);
    IRect changed;
    switch (s.tool) {
        case TileTool::Raise: {
            // Each tile once per stroke: dragging back and forth does not keep stacking.
            const IRect r = brush.clipped(d.width(), d.height());
            const int delta = (s.invert ? -1 : 1) * std::max(1, tile_brush_.strength);
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    std::uint8_t& done = s.touched[static_cast<std::size_t>(y * d.width() + x)];
                    if (done) continue;
                    done = 1;
                    changed.include(tile_raise(d, IRect{x, y, x + 1, y + 1}, delta));
                }
            }
            break;
        }
        case TileTool::Flatten:   changed = tile_set_height(d, brush, s.flatten_to); break;
        case TileTool::SetHeight: changed = tile_set_height(d, brush, tile_brush_.target_height); break;
        case TileTool::Paint:     changed = tile_paint(d, brush, tile_brush_.paint_kind, tile_brush_.paint_side); break;
        case TileTool::Smooth:    changed = tile_smooth(d, brush); break;
        case TileTool::Water:     changed = tile_water(d, brush, tile_brush_.water_level, !s.invert); break;
        case TileTool::Select:    break;
    }
    s.rect.include(changed);
}

void EditorApp::tile_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl, bool, bool double_click) {
    TerrainComponent* t = tile_live_();
    glm::ivec2 tile;
    if (t == nullptr || !tile_hit_(vp, px, tile)) return;
    const std::shared_ptr<TileMapData>& data = t->tiles;
    if (tile_sel_.width() != data->width() || tile_sel_.height() != data->height()) tile_sel_.reset(data->width(), data->height());

    TileStroke& s = tile_stroke_;
    s = TileStroke{};
    if (tile_brush_.tool == TileTool::Select) {
        if (double_click) {
            if (!shift && !ctrl) tile_sel_.clear();
            tile_select_similar(*data, tile, tile_brush_.similar_by_kind, tile_sel_);
            return;
        }
        s.active = true;
        s.selecting = true;
        s.remove = ctrl;
        s.invert = shift;   // add to the selection
        s.sel_start = tile;
        s.last_tile = tile;
        return;
    }
    // Set Height: Ctrl-click samples a tile instead of editing.
    if (tile_brush_.tool == TileTool::SetHeight && ctrl) {
        tile_brush_.target_height = data->at(tile.x, tile.y).steps;
        log_info("Target Height set to " + std::to_string(tile_brush_.target_height) + " steps");
        return;
    }
    s.active = true;
    s.tool = shift ? TileTool::Smooth : tile_brush_.tool;
    s.invert = ctrl;
    s.data = data;
    s.start = data->records();
    s.touched.assign(s.start.size(), 0);
    s.flatten_to = data->at(tile.x, tile.y).steps;
    s.last_tile = tile;
    tile_dab_(tile);
}

void EditorApp::tile_continue_stroke_(const ViewProj& vp, glm::vec2 px) {
    TileStroke& s = tile_stroke_;
    glm::ivec2 tile;
    if (!tile_hit_(vp, px, tile) || tile == s.last_tile) return;
    if (s.selecting) { s.last_tile = tile; return; }
    // Every tile on the way, so a fast drag leaves no gaps.
    glm::ivec2 p = s.last_tile;
    const glm::ivec2 dlt = tile - p;
    const int n = std::max(std::abs(dlt.x), std::abs(dlt.y));
    for (int i = 1; i <= n; ++i) {
        const glm::ivec2 q = s.last_tile + glm::ivec2(glm::round(glm::vec2(dlt) * (static_cast<float>(i) / static_cast<float>(n))));
        if (q != p) tile_dab_(q);
        p = q;
    }
    s.last_tile = tile;
}

void EditorApp::tile_end_stroke_() {
    TileStroke& s = tile_stroke_;
    if (!s.active) return;
    s.active = false;
    if (s.selecting) {
        const IRect r{std::min(s.sel_start.x, s.last_tile.x), std::min(s.sel_start.y, s.last_tile.y),
                      std::max(s.sel_start.x, s.last_tile.x) + 1, std::max(s.sel_start.y, s.last_tile.y) + 1};
        if (!s.invert && !s.remove) tile_sel_.clear();
        tile_sel_.set_rect(r, !s.remove);
        s = TileStroke{};
        return;
    }
    if (s.data && !s.rect.empty()) tile_push_undo_(std::string("Tiles ") + tile_tool_name(s.tool), s.data, s.start, s.rect);
    s = TileStroke{};
}

void EditorApp::tile_push_undo_(const std::string& label, const std::shared_ptr<TileMapData>& data,
                                const std::vector<TileRecord>& whole_before, const IRect& rect) {
    const IRect r = rect.clipped(data->width(), data->height());
    if (r.empty() || whole_before.size() != data->records().size()) return;
    TilePatch before;
    before.data = data;
    before.uid = data->uid();
    before.rect = r;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) before.records.push_back(whole_before[static_cast<std::size_t>(y * data->width() + x)]);
    TilePatch after = capture_tile_patch(data, r);
    tile_undo_.set_budget(256u << 20, [](const TilePatch& p) { return p.bytes(); }, 4);
    tile_undo_.push(label, std::move(before), std::move(after));
}

void EditorApp::tile_apply_undo_(bool redo) {
    const TilePatch* p = redo ? tile_undo_.redo() : tile_undo_.undo();
    if (p != nullptr && !apply_tile_patch(*p)) log_warn("Tile undo skipped: the terrain was rebuilt or removed since");
}

void EditorApp::tile_edit_selection_(const std::string& label, const std::function<void(TileRecord&)>& edit,
                                     const std::string& merge) {
    TerrainComponent* t = tile_live_();
    if (t == nullptr || !t->tiles || tile_sel_.empty()) return;
    const IRect bounds = tile_sel_.bounds();
    TilePatch before = capture_tile_patch(t->tiles, bounds);
    if (::toy::editor::tile_edit_selection(*t->tiles, tile_sel_, edit).empty()) return;
    TilePatch after = capture_tile_patch(t->tiles, bounds);
    tile_undo_.set_budget(256u << 20, [](const TilePatch& p) { return p.bytes(); }, 4);
    tile_undo_.push(label, std::move(before), std::move(after), merge);
}

void EditorApp::tile_regenerate_(ObjectId id) {
    if (id == 0) id = tile_object_;
    coopa::scene::SceneObject* live = sync_.live(id);
    TerrainComponent* t = live ? live->get_component<TerrainComponent>() : nullptr;
    if (t == nullptr || !t->tiles) return;
    tile_regen_data_ = t->tiles;
    tile_regen_before_ = t->tiles->records();
    tile_regen_w_ = t->tiles->width();
    tile_regen_h_ = t->tiles->height();
    tile_regen_wait_ = t->tiles->bakes_done();
    t->tiles->request_bake();
    tile_sel_.clear();
    log_info("Regenerating the world from the generator settings...");
}

void EditorApp::save_tile_maps_() {
    const int n = toy::world::TileMapStore::instance().save_dirty();
    if (n > 0) log_info("Saved " + std::to_string(n) + " tile terrain" + (n == 1 ? "" : "s"));
}

// =================================================================================
// Input / overlay
// =================================================================================

void EditorApp::handle_tile_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool shift = has(in.mods, Mods::Shift);
    const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
    const bool alt = has(in.mods, Mods::Alt);
    glm::ivec2 tile;
    tile_hover_valid_ = hovered && tile_hit_(vp, m, tile);
    if (tile_hover_valid_) tile_hover_ = tile;

    if (tile_stroke_.active) {
        if (!in.down[0]) { tile_end_stroke_(); return; }
        if (in.mouse_delta != glm::vec2(0.0f)) tile_continue_stroke_(vp, m);
        return;
    }
    if (hovered && in.pressed[0] && !alt) {
        const double now = tile_now();
        const bool dbl = now - tile_last_click_ < 0.35 && tile_hover_valid_;
        tile_last_click_ = dbl ? -1.0 : now;
        tile_begin_stroke_(vp, m, shift, ctrl, alt, dbl);
    }
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    viewport_keymap_(ctx, vp, false, false);
    if (ctx.shortcut(Key::LeftBracket)) tile_brush_.size = std::max(1, tile_brush_.size - 1);
    if (ctx.shortcut(Key::RightBracket)) tile_brush_.size = std::min(64, tile_brush_.size + 1);
    const Key digits[] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7};
    for (int i = 0; i < k_tile_tool_count; ++i) {
        if (ctx.shortcut(digits[i])) tile_brush_.tool = static_cast<TileTool>(i);
    }
    if (ctx.shortcut(Key::A, Mods::Alt)) tile_sel_.clear();
    else if (ctx.shortcut(Key::A)) {
        if (const TerrainComponent* t = tile_live_(); t && t->tiles) {
            tile_sel_.reset(t->tiles->width(), t->tiles->height());
            tile_sel_.set_rect(IRect{0, 0, t->tiles->width(), t->tiles->height()}, true);
        }
    }
    if (ctx.shortcut(Key::Escape)) {
        if (!tile_sel_.empty()) tile_sel_.clear();
        else exit_tile_mode_();
    }
    if (ctx.shortcut(Key::Tab)) exit_tile_mode_();
}

void EditorApp::draw_tile_overlay_(imm::Context& ctx, const ViewProj& vp) {
    const TerrainComponent* t = tile_live_();
    if (t == nullptr || !t->tiles || t->tiles->empty()) return;
    const TileMapData& d = *t->tiles;
    const float lift = 0.04f / std::max(0.01f, t->params.height_step);   // just above the top, in steps
    auto top_of = [&](int x, int y) { return static_cast<float>(std::max<int>(0, d.get(x, y).steps)) + lift; };
    auto edge = [&](int x0, int y0, int x1, int y1, float steps, const glm::vec4& c, float w) {
        auto a = vp.project(tile_corner_(*t, x0, y0, steps));
        auto b = vp.project(tile_corner_(*t, x1, y1, steps));
        if (a && b) ctx.line(*a, *b, c, w);
    };
    // The outline of a set of tiles: only the edges between an inside and an outside tile, each at
    // its own tile's top -- so it follows the terraces.
    auto outline = [&](const IRect& bounds, const std::function<bool(int, int)>& in, const glm::vec4& c, float w) {
        const IRect b = bounds.clipped(d.width(), d.height());
        for (int y = b.y0; y < b.y1; ++y) {
            for (int x = b.x0; x < b.x1; ++x) {
                if (!in(x, y)) continue;
                const float z = top_of(x, y);
                if (!in(x, y - 1)) edge(x, y, x + 1, y, z, c, w);
                if (!in(x, y + 1)) edge(x, y + 1, x + 1, y + 1, z, c, w);
                if (!in(x - 1, y)) edge(x, y, x, y + 1, z, c, w);
                if (!in(x + 1, y)) edge(x + 1, y, x + 1, y + 1, z, c, w);
            }
        }
    };

    // The selection.
    if (!tile_sel_.empty()) {
        const glm::vec4 c = imm::with_alpha(ctx.style.accent, 0.95f);
        outline(tile_sel_.bounds(), [this](int x, int y) { return tile_sel_.contains(x, y); }, c, 1.8f);
    }
    // A select drag's rectangle.
    if (tile_stroke_.active && tile_stroke_.selecting) {
        const IRect r{std::min(tile_stroke_.sel_start.x, tile_stroke_.last_tile.x), std::min(tile_stroke_.sel_start.y, tile_stroke_.last_tile.y),
                      std::max(tile_stroke_.sel_start.x, tile_stroke_.last_tile.x) + 1,
                      std::max(tile_stroke_.sel_start.y, tile_stroke_.last_tile.y) + 1};
        const glm::vec4 c = tile_stroke_.remove ? glm::vec4(1.0f, 0.45f, 0.4f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
        outline(r, [&r](int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }, c, 1.4f);
        return;
    }
    if (!tile_hover_valid_) return;
    // The brush (or, selecting, the hovered tile).
    const IRect r = tile_brush_.tool == TileTool::Select ? IRect{tile_hover_.x, tile_hover_.y, tile_hover_.x + 1, tile_hover_.y + 1}
                                                         : tile_brush_rect(tile_hover_, tile_brush_.size);
    const bool invert = tile_stroke_.active ? tile_stroke_.invert : false;
    const glm::vec4 c = invert ? glm::vec4(0.55f, 0.75f, 1.0f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
    outline(r, [&r](int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }, c, 1.5f);
}

// =================================================================================
// UI
// =================================================================================

float EditorApp::draw_tile_header_(imm::Context& ctx, float x, const imm::Box& hb) {
    const float bh = hb.h - 6;
    auto label = [&](const char* text) {
        const float w = ctx.text_width(text) + 6;
        ctx.text_in({x, hb.y, w, hb.h}, text, ctx.style.text_dim, 0.0f);
        x += w;
    };
    label(tile_tool_name(tile_brush_.tool));
    x += 6;
    if (tile_brush_.tool != TileTool::Select) {
        label("Size");
        float size = static_cast<float>(tile_brush_.size);
        if (ctx.drag_float_box("tile_size", {x, hb.y + 3, 56, bh}, &size, 0.1f, 1.0f, 64.0f, "%.0f")) {
            tile_brush_.size = static_cast<int>(std::lround(size));
        }
        ctx.tooltip("Size\nBrush edge in tiles ([ and ])");
        x += 62;
    }
    if (tile_brush_.tool == TileTool::Raise) {
        label("Steps");
        float st = static_cast<float>(tile_brush_.strength);
        if (ctx.drag_float_box("tile_strength", {x, hb.y + 3, 46, bh}, &st, 0.05f, 1.0f, 50.0f, "%.0f")) {
            tile_brush_.strength = static_cast<int>(std::lround(st));
        }
        ctx.tooltip("Steps\nHow many steps each tile rises (or, with Ctrl, sinks) per stroke");
        x += 52;
    }
    return x + 8;
}

void EditorApp::draw_tile_toolbar_(imm::Context& ctx, const imm::Box& r) {
    float y = r.y + 4;
    for (const auto& b : k_tile_tools) {
        const imm::Box bb{r.x + 3, y, kToolSize, kToolSize};
        y += kToolSize + 2;
        if (ctx.icon_button(std::string("tile_") + tile_tool_name(b.tool), b.icon, b.tip, tile_brush_.tool == b.tool, kToolSize,
                            imm::Context::kAll, bb)) {
            tile_brush_.tool = b.tool;
        }
    }
}

void EditorApp::draw_tile_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    const glm::vec4 dim = ctx.style.text_dim;
    TerrainComponent* t = tile_live_();
    const Node* obj = doc_.find(tile_object_);
    ctx.label(std::string("Tile Terrain: ") + (obj ? get_string(*obj, "name", "?") : "?"));
    if (ctx.button("Exit Tile Mode", -1, true, I::ObjectMode)) { exit_tile_mode_(); return; }
    ctx.tooltip("Exit Tile Mode (Tab)");
    if (t == nullptr || !t->tiles || t->tiles->empty()) {
        ctx.label(t && t->baking() ? "Generating the world..." : "The terrain is still loading.", &dim);
        return;
    }
    TileMapData& d = *t->tiles;

    // --- Tools ---
    if (ctx.collapsing_header("Tools", true, nullptr, I::Tool)) {
        for (int i = 0; i < k_tile_tool_count; ++i) {
            const ToolButton& b = k_tile_tools[i];
            char label[48];
            std::snprintf(label, sizeof(label), "%s  (%d)", tile_tool_name(b.tool), i + 1);
            if (ctx.selectable(label, tile_brush_.tool == b.tool, 0.0f, b.icon)) tile_brush_.tool = b.tool;
            ctx.tooltip(b.tip);
        }
    }

    // --- Brush / Select options ---
    if (tile_brush_.tool == TileTool::Select) {
        if (ctx.collapsing_header("Select", true, nullptr, I::SelectBox)) {
            ctx.paragraph("Drag a rectangle; Shift adds, Ctrl removes. Double-click selects the connected tiles that match. "
                          "A selects everything, Alt A or Esc clears.", &dim);
            int by = tile_brush_.similar_by_kind ? 1 : 0;
            if (ctx.combo("Double-Click Matches", &by, {"Same height", "Same surface"})) tile_brush_.similar_by_kind = by == 1;
            if (ctx.button("Select All", 120, true, I::SelectBox)) {
                tile_sel_.reset(d.width(), d.height());
                tile_sel_.set_rect(IRect{0, 0, d.width(), d.height()}, true);
            }
            ctx.same_line();
            if (ctx.button("Clear", 90, !tile_sel_.empty(), I::X)) tile_sel_.clear();
        }
    } else if (ctx.collapsing_header("Brush", true, nullptr, I::BrushDraw)) {
        ctx.drag_int("Size (tiles)", &tile_brush_.size, 0.1f, 1, 64);
        ctx.tooltip("Size\nThe square brush's edge, in tiles ([ and ])");
        switch (tile_brush_.tool) {
            case TileTool::Raise:
                ctx.drag_int("Steps per Tile", &tile_brush_.strength, 0.05f, 1, 50);
                ctx.paragraph("Each tile under the brush rises once per stroke. Hold Ctrl to lower; Shift smooths.", &dim);
                break;
            case TileTool::Flatten:
                ctx.paragraph("Levels the brush to the height of the tile the stroke starts on.", &dim);
                break;
            case TileTool::SetHeight:
                ctx.drag_int("Target Height (steps)", &tile_brush_.target_height, 0.1f, 0, 2000);
                ctx.paragraph("Ctrl-click a tile to pick its height.", &dim);
                break;
            case TileTool::Paint: {
                int k = static_cast<int>(tile_brush_.paint_kind);
                if (ctx.combo("Surface", &k, kind_options())) tile_brush_.paint_kind = static_cast<TileKind>(k);
                ctx.checkbox("Also Paint Sides", &tile_brush_.paint_side);
                ctx.tooltip("Also Paint Sides\nThe cliff under the top takes the surface's usual soil (grass over dirt, ...)");
                break;
            }
            case TileTool::Smooth:
                ctx.paragraph("Moves each tile one step toward its neighbours' median height.", &dim);
                break;
            case TileTool::Water:
                ctx.drag_int("Water Level (steps)", &tile_brush_.water_level, 0.1f, 0, 2000);
                ctx.paragraph("Floods tiles lower than the level; Ctrl drains water back down to its bed.", &dim);
                break;
            case TileTool::Select: break;
        }
    }

    draw_tile_selection_section_(ctx, *t);

    // --- Hovered tile ---
    if (ctx.collapsing_header("Tile Under Cursor", false, nullptr, I::Info)) {
        if (tile_hover_valid_ && d.contains(tile_hover_.x, tile_hover_.y)) {
            const TileRecord& r = d.at(tile_hover_.x, tile_hover_.y);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Tile %d, %d  --  height %d steps", tile_hover_.x, tile_hover_.y, r.steps);
            ctx.label(buf);
            std::snprintf(buf, sizeof(buf), "Surface %s over %s", k_kind_names[static_cast<int>(r.top_kind()) % 12],
                          k_kind_names[static_cast<int>(r.side_kind()) % 12]);
            ctx.label(buf, &dim);
            const std::string biome(coopa::maps::biome_name(static_cast<coopa::maps::Biome>(r.biome)));
            std::snprintf(buf, sizeof(buf), "Biome %s  --  temperature %.2f, moisture %.2f", biome.c_str(), r.temperature01(),
                          r.moisture01());
            ctx.label(buf, &dim);
            std::string flags;
            if (r.water()) flags += "water (" + std::to_string(r.water_depth) + " deep) ";
            if (r.has(TileRecord::Ocean)) flags += "ocean ";
            if (r.has(TileRecord::Coast)) flags += "coast ";
            if (r.has(TileRecord::River)) flags += "river ";
            if (r.region != TileRecord::k_no_id) flags += "region " + std::to_string(r.region) + " ";
            if (r.country != TileRecord::k_no_id) flags += "country " + std::to_string(r.country);
            if (!flags.empty()) ctx.label(flags, &dim);
        } else {
            ctx.label("Hover the terrain.", &dim);
        }
    }

    // --- Generate ---
    if (ctx.collapsing_header("Generate", false, nullptr, I::World)) {
        ctx.paragraph("The tiles were generated from the TileTerrain's Generator settings (seed, shape, sea level...), then "
                      "edited. Regenerate replaces every tile with a fresh world from the current settings -- your edits "
                      "included. It can be undone.",
                      &dim);
        if (ctx.button(t->baking() ? "Regenerating..." : "Regenerate World", -1, !t->baking() && !tile_regen_data_, I::Restart)) {
            tile_regenerate_();
        }
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d x %d tiles%s", d.width(), d.height(), d.dirty_on_disk() ? "  --  unsaved (Ctrl S)" : "");
    ctx.label(buf, &dim);
}

void EditorApp::draw_tile_selection_section_(imm::Context& ctx, TerrainComponent& t) {
    using I = imm::Icon;
    const glm::vec4 dim = ctx.style.text_dim;
    if (tile_sel_.empty() || !t.tiles) return;
    if (!ctx.collapsing_header("Selected Tiles", true, nullptr, I::SelectBox)) return;
    const TileMapData& d = *t.tiles;

    // What the selected tiles share: a value when they all agree, "mixed" when not.
    struct Common {
        bool first = true, mixed_steps = false, mixed_top = false, mixed_side = false, mixed_biome = false;
        bool mixed_region = false, mixed_country = false;
        int steps = 0, water = 0;
        TileKind top{}, side{};
        int biome = 0, region = 0, country = 0;
        double temperature = 0.0, moisture = 0.0;
    } c;
    tile_sel_.for_each([&](int x, int y) {
        if (!d.contains(x, y)) return;
        const TileRecord& r = d.at(x, y);
        if (c.first) {
            c.first = false;
            c.steps = r.steps; c.top = r.top_kind(); c.side = r.side_kind(); c.biome = r.biome;
            c.region = r.region; c.country = r.country;
        } else {
            c.mixed_steps = c.mixed_steps || r.steps != c.steps;
            c.mixed_top = c.mixed_top || r.top_kind() != c.top;
            c.mixed_side = c.mixed_side || r.side_kind() != c.side;
            c.mixed_biome = c.mixed_biome || r.biome != c.biome;
            c.mixed_region = c.mixed_region || r.region != c.region;
            c.mixed_country = c.mixed_country || r.country != c.country;
        }
        c.temperature += r.temperature01();
        c.moisture += r.moisture01();
        c.water += r.water() ? 1 : 0;
    });
    const int n = tile_sel_.count();
    c.temperature /= std::max(1, n);
    c.moisture /= std::max(1, n);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d tile%s selected", n, n == 1 ? "" : "s");
    ctx.label(buf, &dim);

    // Height.
    int steps = c.steps;
    if (ctx.drag_int(c.mixed_steps ? "Height (mixed)" : "Height (steps)", &steps, 0.1f, 0, 2000)) {
        tile_edit_selection_("Set Tile Height", [steps](TileRecord& r) {
            if (r.water()) r.water_depth = static_cast<std::uint8_t>(std::clamp(r.steps - steps, 0, 255));
            else r.steps = static_cast<std::int16_t>(steps);
        }, "tile_height");
    }
    ctx.tooltip("Height\nSets every selected tile to this height (water keeps its surface, its bed moves)");
    if (ctx.button("-1 Step", 90, true, I::ArrowDown)) tile_edit_selection_("Lower Tiles", [](TileRecord& r) {
        if (!r.water()) r.steps = static_cast<std::int16_t>(std::max(0, r.steps - 1));
    });
    ctx.same_line();
    if (ctx.button("+1 Step", 90, true, I::Plus)) tile_edit_selection_("Raise Tiles", [](TileRecord& r) {
        if (!r.water()) r.steps = static_cast<std::int16_t>(r.steps + 1);
    });

    // Surfaces.
    auto kind_combo = [&](const char* label, TileKind cur, bool mixed, bool top) {
        std::vector<std::string> opts = kind_options();
        int k = static_cast<int>(cur);
        if (mixed) { opts.push_back("(mixed)"); k = static_cast<int>(opts.size()) - 1; }
        const int was = k;
        if (ctx.combo(label, &k, opts) && k != was && k < static_cast<int>(toy::world::k_tile_kind_count)) {
            const TileKind kind = static_cast<TileKind>(k);
            tile_edit_selection_(top ? "Set Tile Surface" : "Set Tile Sides", [kind, top](TileRecord& r) {
                if (top) r.set_top_kind(kind); else r.set_side_kind(kind);
            });
        }
    };
    kind_combo("Top Surface", c.top, c.mixed_top, true);
    kind_combo("Side Surface", c.side, c.mixed_side, false);

    // Biome and climate (mapcoopa's).
    {
        std::vector<std::string> opts = biome_options();
        int b = c.biome;
        if (c.mixed_biome) { opts.push_back("(mixed)"); b = static_cast<int>(opts.size()) - 1; }
        const int was = b;
        if (ctx.combo("Biome", &b, opts) && b != was && b < static_cast<int>(coopa::maps::k_biome_count)) {
            tile_edit_selection_("Set Tile Biome", [b](TileRecord& r) { r.biome = static_cast<std::uint8_t>(b); });
        }
    }
    float temp = static_cast<float>(c.temperature);
    if (ctx.slider_float("Temperature", &temp, 0.0f, 1.0f, "%.2f")) {
        tile_edit_selection_("Set Tile Temperature", [temp](TileRecord& r) { r.temperature = TileRecord::quantise01(temp); }, "tile_temperature");
    }
    ctx.tooltip("Temperature\nmapcoopa's yearly mean: 0 polar, 1 equatorial (the average is shown when tiles differ)");
    float moist = static_cast<float>(c.moisture);
    if (ctx.slider_float("Moisture", &moist, 0.0f, 1.0f, "%.2f")) {
        tile_edit_selection_("Set Tile Moisture", [moist](TileRecord& r) { r.moisture = TileRecord::quantise01(moist); }, "tile_moisture");
    }

    // Water.
    bool water = c.water == n;
    if (ctx.checkbox(c.water > 0 && c.water < n ? "Water (some)" : "Water", &water)) {
        tile_edit_selection_(water ? "Make Water" : "Drain Water", [water](TileRecord& r) {
            if (water && !r.water()) {
                r.set(TileRecord::Water, true);
                r.water_depth = 1;
                r.set_top_kind(TileKind::Water);
                r.set_side_kind(TileKind::Water);
            } else if (!water && r.water()) {
                r.steps = static_cast<std::int16_t>(std::max(0, r.steps - r.water_depth));
                r.water_depth = 0;
                r.set(TileRecord::Water, false);
                r.set(TileRecord::Ocean, false);
                r.set_top_kind(TileKind::Sand);
                r.set_side_kind(TileKind::Sand);
            }
        });
    }
    ctx.tooltip("Water\nOn: the tiles become a water surface at their height, one step deep. Off: water drains down to its bed");

    // Region / country ids.
    int region = c.region == TileRecord::k_no_id ? -1 : c.region;
    if (ctx.drag_int(c.mixed_region ? "Region (mixed)" : "Region", &region, 0.05f, -1, 65534)) {
        tile_edit_selection_("Set Tile Region", [region](TileRecord& r) {
            r.region = region < 0 ? TileRecord::k_no_id : static_cast<std::uint16_t>(region);
        }, "tile_region");
    }
    int country = c.country == TileRecord::k_no_id ? -1 : c.country;
    if (ctx.drag_int(c.mixed_country ? "Country (mixed)" : "Country", &country, 0.05f, -1, 65534)) {
        tile_edit_selection_("Set Tile Country", [country](TileRecord& r) {
            r.country = country < 0 ? TileRecord::k_no_id : static_cast<std::uint16_t>(country);
        }, "tile_country");
    }
    // A drag's merged step closes when the mouse lets go.
    if (!ctx.input().down[0]) tile_undo_.end_merge();
}

} // namespace editor
} // namespace toy
