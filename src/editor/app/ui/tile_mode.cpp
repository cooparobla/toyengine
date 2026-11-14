#include "editor/app/ui/tile_mode.h"
#include "editor/app/ui/viewport_chrome.h"
#include "editor/app/ui/terrain.h"

#include "editor/app/editor_app.h"
#include "editor/app/ui/terrain_common.h"
#include "editor/app/ui/world_generator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>

#include <coopa/maps/biome.h>
#include <toyengine/world/terra_store.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_grid.h>
#include <toyengine/world/terrain_water.h>

namespace toy {
namespace editor {

bool TileModePanel::is_tile_terrain_object_(ObjectId id) const {
    return id != 0 && terrain_mode_of(app_.doc_, id) == toy::world::TerrainMode::Tiles;
}

void TileModePanel::set_tile_mode(bool on) {
    if (on) enter_tile_mode_(app_.doc_.primary()); else exit_tile_mode_();
}

using toy::world::IRect;
using toy::world::TerraData;
using toy::world::TerrainComponent;
using toy::world::TerrainField;
using toy::world::TileCell;
using toy::world::TileKind;
using toy::world::WaterKind;

namespace {

/** @brief Moves a tile's ground to `steps` (never below 0); water over it keeps its absolute
 *         level, draining if the new ground has risen through it. */
void move_ground(TileCell& r, int steps, float height_step) {
    r.steps = static_cast<std::int16_t>(std::clamp(steps, 0, 32000));
    if (r.wet() && r.water_level <= static_cast<float>(r.steps) * height_step) {
        r.water_kind = static_cast<std::uint8_t>(WaterKind::None);
        r.water_level = 0.0f;
        r.flow = 0;
        r.flow_authored = 0;
    }
}

/** @brief Sets a tile's water to the terrace `level_steps` (at or under the ground: drains),
 *         keeping its Ocean-ness as it was. */
void set_water_level(TileCell& r, int level_steps, float height_step) {
    const float level_m = static_cast<float>(level_steps) * height_step;
    const float ground = static_cast<float>(r.steps) * height_step;
    const bool wet = level_m > ground;
    const bool was_ocean = r.water_kind == static_cast<std::uint8_t>(WaterKind::Ocean);
    r.water_kind = static_cast<std::uint8_t>(wet ? (was_ocean ? WaterKind::Ocean : WaterKind::Lake) : WaterKind::None);
    r.water_level = wet ? level_m : 0.0f;
    if (!wet) { r.flow = 0; r.flow_authored = 0; }
}

double tile_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Unsized on purpose, with the assert below: a sized array leaves a null in the gap when a kind
// is added to TileKind and forgotten here, and kind_options() then throws building a string from
// it -- which is a crash in the Tile Mode panel, not a compile error.
const char* const k_kind_names[] = {"Grass", "Dirt", "Stone", "Sand", "Snow", "Rock",
                                    "Water", "Ice",  "Moss",  "Clay", "Ash",  "Salt", "Road"};
static_assert(std::size(k_kind_names) == toy::world::k_tile_kind_count,
              "every TileKind needs a name here -- see tile_types.h's k_tile_kind_count");

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

TerrainComponent* TileModePanel::tile_live_() const {
    if (tile_object_ == 0) return nullptr;
    coopa::scene::SceneObject* live = app_.sync_.live(tile_object_);
    TerrainComponent* t = live ? live->get_component<TerrainComponent>() : nullptr;
    // One component type, two modes: this panel edits the tile one.
    return t != nullptr && t->mode == toy::world::TerrainMode::Tiles ? t : nullptr;
}

void TileModePanel::enter_tile_mode_(ObjectId id) {
    if (!is_tile_terrain_object_(id) || app_.playing()) {
        app_.log_warn("Tile Mode: select an object with a TileTerrain component first");
        return;
    }
    if (app_.terrain_panel_->in_terrain_mode_()) app_.terrain_panel_->exit_terrain_mode_();
    if (app_.interaction_mode() != InteractionMode::Object) app_.set_interaction_mode(InteractionMode::Object);
    tile_mode_ = true;
    tile_object_ = id;
    app_.tool_ = EditorApp::Tool::Select;
    app_.prop_tab_ = PropTab::Tool;
    tile_sel_.reset(0, 0);
    app_.doc_.clear_selection();
    app_.doc_.select(id, true);
}

void TileModePanel::exit_tile_mode_() {
    if (tile_stroke_.active) tile_end_stroke_();
    tile_stroke_ = TileStroke{};
    tile_mode_ = false;
    tile_object_ = 0;
    tile_hover_valid_ = false;
}

void TileModePanel::tile_frame_() {
    // A Regenerate lands a few frames after it was asked for: record its undo step then.
    if (tile_regen_data_ && tile_regen_data_->bakes_done() > tile_regen_wait_) {
        auto data = tile_regen_data_;
        tile_regen_data_.reset();
        if (data->geometry().tiles_x == tile_regen_w_ && data->geometry().tiles_y == tile_regen_h_) {
            tile_push_undo_("Regenerate World", data, tile_regen_before_,
                            IRect{0, 0, data->geometry().tiles_x, data->geometry().tiles_y});
        } else {
            // The world changed size: the step holds the whole grid either side, with its size.
            TilePatch before;
            before.data = data;
            before.uid = data->uid();
            before.width = tile_regen_w_;
            before.height = tile_regen_h_;
            before.rect = IRect{0, 0, tile_regen_w_, tile_regen_h_};
            before.records = std::move(tile_regen_before_);
            TilePatch after = capture_tile_patch(data, IRect{0, 0, data->geometry().tiles_x, data->geometry().tiles_y});
            after.width = data->geometry().tiles_x;
            after.height = data->geometry().tiles_y;
            tile_undo_.set_budget(256u << 20, [](const TilePatch& p) { return p.bytes(); }, 4);
            tile_undo_.push("Regenerate World", std::move(before), std::move(after));
        }
        tile_regen_before_.clear();
        app_.log_info("World regenerated");
    }
    if (!tile_mode_) return;
    if (app_.playing() || !app_.doc_.find(tile_object_) || !is_tile_terrain_object_(tile_object_)) exit_tile_mode_();
}

// =================================================================================
// Strokes
// =================================================================================

IRect TileModePanel::tile_face_clip_(const IRect& r, const glm::ivec2& anchor) const {
    const TerrainComponent* t = tile_live_();
    if (t == nullptr || t->topology == nullptr) return r;
    return t->topology->clip_brush(t->params, r, anchor);
}

bool TileModePanel::tile_hit_(const ViewProj& vp, glm::vec2 px, glm::ivec2& tile, glm::vec3* world) const {
    const TerrainComponent* t = tile_live_();
    if (t == nullptr || t->field.empty() || t->owner == nullptr) return false;
    glm::vec3 o, d;
    vp.ray(px, o, d);
    const glm::mat4 m = t->owner->get_transform() ? t->owner->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
    // The march itself is the engine's (TerrainComponent::raycast_tiles), whatever the shape,
    // so the editor and a game's digging tool pick exactly the same tile.
    toy::world::TileHit hit;
    if (!t->raycast_tiles(o, d, 1e5f, hit)) return false;
    tile = hit.tile;
    if (world) *world = glm::vec3(m * glm::vec4(hit.point, 1.0f));
    return true;
}

glm::vec3 TileModePanel::tile_corner_(const TerrainComponent& t, int x, int y, float steps, int face) const {
    const glm::mat4 m = t.owner && t.owner->get_transform() ? t.owner->get_transform()->transform().get_world_matrix() : glm::mat4(1.0f);
    if (t.topology != nullptr) {
        const glm::vec3 p = t.topology->grid_point(t.params, x, y,
                                                   static_cast<double>(steps) * t.params.height_step, face);
        return glm::vec3(m * glm::vec4(p, 1.0f));
    }
    return glm::vec3(m * glm::vec4(static_cast<float>(x) * t.params.tile_size, static_cast<float>(y) * t.params.tile_size,
                                   steps * t.params.height_step, 1.0f));
}

void TileModePanel::tile_dab_(const glm::ivec2& tile) {
    TileStroke& s = tile_stroke_;
    if (!s.data) return;
    TerrainComponent* t = tile_live_();
    if (t == nullptr) return;
    TerraData& d = *s.data;
    TerrainField& field = t->field;
    const IRect brush = tile_face_clip_(tile_brush_rect(tile, tile_brush_.size), tile);
    IRect changed;
    switch (s.tool) {
        case TileTool::Raise: {
            // Each tile once per stroke: dragging back and forth does not keep stacking.
            const IRect r = brush.clipped(d.geometry().tiles_x, d.geometry().tiles_y);
            const int delta = (s.invert ? -1 : 1) * std::max(1, tile_brush_.strength);
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    std::uint8_t& done = s.touched[static_cast<std::size_t>(y * d.geometry().tiles_x + x)];
                    if (done) continue;
                    done = 1;
                    changed.include(tile_raise(field, IRect{x, y, x + 1, y + 1}, delta));
                }
            }
            break;
        }
        case TileTool::Flatten:   changed = tile_set_height(field, brush, s.flatten_to); break;
        case TileTool::SetHeight: changed = tile_set_height(field, brush, tile_brush_.target_height); break;
        case TileTool::Paint:     changed = tile_paint(field, brush, tile_brush_.paint_kind, tile_brush_.paint_side); break;
        case TileTool::Smooth:    changed = tile_smooth(field, brush); break;
        case TileTool::Water:     changed = tile_water(field, brush, tile_brush_.water_level, !s.invert); break;
        case TileTool::Select:    break;
    }
    s.rect.include(changed);
}

void TileModePanel::tile_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool shift, bool ctrl, bool, bool double_click) {
    TerrainComponent* t = tile_live_();
    glm::ivec2 tile;
    if (t == nullptr || !tile_hit_(vp, px, tile)) return;
    const std::shared_ptr<TerraData>& data = t->field.data_shared();
    if (tile_sel_.width() != data->geometry().tiles_x || tile_sel_.height() != data->geometry().tiles_y) {
        tile_sel_.reset(data->geometry().tiles_x, data->geometry().tiles_y);
    }

    TileStroke& s = tile_stroke_;
    s = TileStroke{};
    if (tile_brush_.tool == TileTool::Select) {
        if (double_click) {
            if (!shift && !ctrl) tile_sel_.clear();
            tile_select_similar(t->field, tile, tile_brush_.similar_by_kind, tile_sel_);
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
        app_.log_info("Target Height set to " + std::to_string(tile_brush_.target_height) + " steps");
        return;
    }
    s.active = true;
    s.tool = shift ? TileTool::Smooth : tile_brush_.tool;
    s.invert = ctrl;
    s.data = data;
    s.start = data->tiles();
    s.touched.assign(s.start.size(), 0);
    s.flatten_to = data->at(tile.x, tile.y).steps;
    s.last_tile = tile;
    tile_dab_(tile);
}

void TileModePanel::tile_continue_stroke_(const ViewProj& vp, glm::vec2 px) {
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

void TileModePanel::tile_end_stroke_() {
    TileStroke& s = tile_stroke_;
    if (!s.active) return;
    s.active = false;
    if (s.selecting) {
        const IRect r = tile_face_clip_(IRect{std::min(s.sel_start.x, s.last_tile.x), std::min(s.sel_start.y, s.last_tile.y),
                                              std::max(s.sel_start.x, s.last_tile.x) + 1, std::max(s.sel_start.y, s.last_tile.y) + 1},
                                        s.sel_start);
        if (!s.invert && !s.remove) tile_sel_.clear();
        tile_sel_.set_rect(r, !s.remove);
        s = TileStroke{};
        return;
    }
    if (s.data && !s.rect.empty()) tile_push_undo_(std::string("Tiles ") + tile_tool_name(s.tool), s.data, s.start, s.rect);
    s = TileStroke{};
}

void TileModePanel::tile_push_undo_(const std::string& label, const std::shared_ptr<TerraData>& data,
                                const std::vector<TileCell>& whole_before, const IRect& rect) {
    const int width = data->geometry().tiles_x, height = data->geometry().tiles_y;
    const IRect r = rect.clipped(width, height);
    if (r.empty() || whole_before.size() != data->tiles().size()) return;
    TilePatch before;
    before.data = data;
    before.uid = data->uid();
    before.rect = r;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) before.records.push_back(whole_before[static_cast<std::size_t>(y * width + x)]);
    TilePatch after = capture_tile_patch(data, r);
    tile_undo_.set_budget(256u << 20, [](const TilePatch& p) { return p.bytes(); }, 4);
    tile_undo_.push(label, std::move(before), std::move(after));
}

void TileModePanel::tile_apply_undo_(bool redo) {
    const TilePatch* p = redo ? tile_undo_.redo() : tile_undo_.undo();
    if (p != nullptr && !apply_tile_patch(*p)) app_.log_warn("Tile undo skipped: the terrain was rebuilt or removed since");
}

void TileModePanel::tile_edit_selection_(const std::string& label, const std::function<void(TileCell&)>& edit,
                                     const std::string& merge, bool settle) {
    TerrainComponent* t = tile_live_();
    if (t == nullptr || t->field.empty() || tile_sel_.empty()) return;
    const IRect bounds = tile_sel_.bounds();
    // Water that runs in (or out) past the selection is part of the same edit, as far as this margin.
    constexpr int k_settle_margin = 24;
    const IRect reach = settle ? IRect{bounds.x0 - k_settle_margin, bounds.y0 - k_settle_margin, bounds.x1 + k_settle_margin,
                                       bounds.y1 + k_settle_margin}.clipped(t->field.geometry().tiles_x, t->field.geometry().tiles_y)
                               : bounds;
    const std::shared_ptr<TerraData> data = t->field.data_shared();
    TilePatch before = capture_tile_patch(data, reach);
    if (::toy::editor::tile_edit_selection(t->field, tile_sel_, edit).empty()) return;
    if (settle) {
        t->sync_field();
        toy::world::settle_water(t->field, bounds, reach);
    }
    TilePatch after = capture_tile_patch(data, reach);
    tile_undo_.set_budget(256u << 20, [](const TilePatch& p) { return p.bytes(); }, 4);
    tile_undo_.push(label, std::move(before), std::move(after), merge);
}

void TileModePanel::tile_regenerate_(ObjectId id) {
    if (id == 0) id = tile_object_;
    coopa::scene::SceneObject* live = app_.sync_.live(id);
    TerrainComponent* t = live ? live->get_component<TerrainComponent>() : nullptr;
    if (t != nullptr && t->mode != toy::world::TerrainMode::Tiles) t = nullptr;
    if (t == nullptr || t->field.empty()) return;
    tile_regen_data_ = t->field.data_shared();
    tile_regen_before_ = tile_regen_data_->tiles();
    tile_regen_w_ = tile_regen_data_->geometry().tiles_x;
    tile_regen_h_ = tile_regen_data_->geometry().tiles_y;
    tile_regen_wait_ = tile_regen_data_->bakes_done();
    tile_regen_data_->request_bake();
    tile_sel_.clear();
    app_.log_info("Regenerating the world from the generator settings...");
}

void TileModePanel::save_tile_maps_() {
    const int n = toy::world::TerraStore::instance().save_dirty();
    if (n > 0) app_.log_info("Saved " + std::to_string(n) + " tile terrain" + (n == 1 ? "" : "s"));
}

// =================================================================================
// Input / overlay
// =================================================================================

void TileModePanel::handle_tile_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
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
        // A double-click: two Select clicks on the same tile in quick succession.
        const double now = tile_now();
        const bool select = tile_brush_.tool == TileTool::Select;
        const bool dbl = select && tile_hover_valid_ && tile_hover_ == tile_last_click_tile_ && now - tile_last_click_ < 0.35;
        tile_last_click_ = (dbl || !select) ? -1.0 : now;
        tile_last_click_tile_ = tile_hover_;
        tile_begin_stroke_(vp, m, shift, ctrl, alt, dbl);
    }
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    app_.viewport_keymap_(ctx, vp, false, false);
    if (ctx.shortcut(Key::LeftBracket)) tile_brush_.size = std::max(1, tile_brush_.size - 1);
    if (ctx.shortcut(Key::RightBracket)) tile_brush_.size = std::min(64, tile_brush_.size + 1);
    const Key digits[] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7};
    for (int i = 0; i < k_tile_tool_count; ++i) {
        if (ctx.shortcut(digits[i])) tile_brush_.tool = static_cast<TileTool>(i);
    }
    if (ctx.shortcut(Key::A, Mods::Alt)) tile_sel_.clear();
    else if (ctx.shortcut(Key::A)) {
        if (const TerrainComponent* t = tile_live_(); t && !t->field.empty()) {
            const int w = t->field.geometry().tiles_x, h = t->field.geometry().tiles_y;
            tile_sel_.reset(w, h);
            tile_sel_.set_rect(IRect{0, 0, w, h}, true);
        }
    }
    if (ctx.shortcut(Key::Escape)) {
        if (!tile_sel_.empty()) tile_sel_.clear();
        else exit_tile_mode_();
    }
    if (ctx.shortcut(Key::Tab)) exit_tile_mode_();
}

void TileModePanel::draw_tile_overlay_(imm::Context& ctx, const ViewProj& vp) {
    const TerrainComponent* t = tile_live_();
    if (t == nullptr || t->field.empty()) return;
    const TerraData& d = *t->field.data();
    const float lift = 0.04f / std::max(0.01f, t->params.height_step);   // just above the top, in steps
    auto top_of = [&](int x, int y) { return static_cast<float>(std::max<int>(0, d.get(x, y).steps)) + lift; };
    // Where the grid is not a plane, each edge is bent onto the real surface in a few segments,
    // all within its own tile's region.
    const glm::mat4 m = t->owner && t->owner->get_transform() ? t->owner->get_transform()->transform().get_world_matrix()
                                                               : glm::mat4(1.0f);
    auto surface_point = [&](glm::vec2 xy, float steps, int region) {
        const glm::vec3 p = t->topology->grid_point(t->params, xy.x, xy.y,
                                                    static_cast<double>(steps) * t->params.height_step, region);
        return glm::vec3(m * glm::vec4(p, 1.0f));
    };
    auto edge = [&](int x0, int y0, int x1, int y1, float steps, const glm::vec4& c, float w, int region) {
        if (t->topology != nullptr) {
            constexpr int k_segments = 4;
            const glm::vec2 a0(x0, y0), a1(x1, y1);
            for (int i = 0; i < k_segments; ++i) {
                auto a = vp.project(surface_point(glm::mix(a0, a1, static_cast<float>(i) / k_segments), steps, region));
                auto b = vp.project(surface_point(glm::mix(a0, a1, static_cast<float>(i + 1) / k_segments), steps, region));
                if (a && b) ctx.line(*a, *b, c, w);
            }
            return;
        }
        auto a = vp.project(tile_corner_(*t, x0, y0, steps));
        auto b = vp.project(tile_corner_(*t, x1, y1, steps));
        if (a && b) ctx.line(*a, *b, c, w);
    };
    // The outline of a set of tiles: only the edges between an inside and an outside tile, each at
    // its own tile's top -- so it follows the terraces.
    auto outline = [&](const IRect& bounds, const std::function<bool(int, int)>& in, const glm::vec4& c, float w) {
        const IRect b = bounds.clipped(d.geometry().tiles_x, d.geometry().tiles_y);
        for (int y = b.y0; y < b.y1; ++y) {
            for (int x = b.x0; x < b.x1; ++x) {
                if (!in(x, y)) continue;
                const float z = top_of(x, y);
                const int face = t->topology != nullptr ? t->topology->region_at(t->params, x) : 0;
                if (!in(x, y - 1)) edge(x, y, x + 1, y, z, c, w, face);
                if (!in(x, y + 1)) edge(x, y + 1, x + 1, y + 1, z, c, w, face);
                if (!in(x - 1, y)) edge(x, y, x, y + 1, z, c, w, face);
                if (!in(x + 1, y)) edge(x + 1, y, x + 1, y + 1, z, c, w, face);
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
        const IRect r = tile_face_clip_(IRect{std::min(tile_stroke_.sel_start.x, tile_stroke_.last_tile.x),
                                              std::min(tile_stroke_.sel_start.y, tile_stroke_.last_tile.y),
                                              std::max(tile_stroke_.sel_start.x, tile_stroke_.last_tile.x) + 1,
                                              std::max(tile_stroke_.sel_start.y, tile_stroke_.last_tile.y) + 1},
                                        tile_stroke_.sel_start);
        const glm::vec4 c = tile_stroke_.remove ? glm::vec4(1.0f, 0.45f, 0.4f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
        outline(r, [&r](int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }, c, 1.4f);
        return;
    }
    if (!tile_hover_valid_) return;
    // The brush (or, selecting, the hovered tile).
    const IRect r = tile_brush_.tool == TileTool::Select ? IRect{tile_hover_.x, tile_hover_.y, tile_hover_.x + 1, tile_hover_.y + 1}
                                                         : tile_face_clip_(tile_brush_rect(tile_hover_, tile_brush_.size), tile_hover_);
    const bool invert = tile_stroke_.active ? tile_stroke_.invert : false;
    const glm::vec4 c = invert ? glm::vec4(0.55f, 0.75f, 1.0f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
    outline(r, [&r](int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }, c, 1.5f);
}

// =================================================================================
// UI
// =================================================================================

float TileModePanel::draw_tile_header_(imm::Context& ctx, float x, const imm::Box& hb) {
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

void TileModePanel::draw_tile_toolbar_(imm::Context& ctx, const imm::Box& r) {
    float y = r.y + 4;
    for (const auto& b : k_tile_tools) {
        const imm::Box bb{r.x + 3, y, app_.viewport_chrome_panel_->kToolSize, app_.viewport_chrome_panel_->kToolSize};
        y += app_.viewport_chrome_panel_->kToolSize + 2;
        if (ctx.icon_button(std::string("tile_") + tile_tool_name(b.tool), b.icon, b.tip, tile_brush_.tool == b.tool, app_.viewport_chrome_panel_->kToolSize,
                            imm::Context::kAll, bb)) {
            tile_brush_.tool = b.tool;
        }
    }
}

void TileModePanel::draw_tile_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    const glm::vec4 dim = ctx.style.text_dim;
    TerrainComponent* t = tile_live_();
    const Node* obj = app_.doc_.find(tile_object_);
    ctx.label(std::string("Tile Terrain: ") + (obj ? get_string(*obj, "name", "?") : "?"));
    if (ctx.button("Exit Tile Mode", -1, true, I::ObjectMode)) { exit_tile_mode_(); return; }
    ctx.tooltip("Exit Tile Mode (Tab)");
    if (t == nullptr || t->field.empty()) {
        ctx.label(t && t->baking() ? "Generating the world..." : "The terrain is still loading.", &dim);
        return;
    }
    TerraData& d = *t->field.data();
    const float hstep = t->params.height_step;

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
                tile_sel_.reset(d.geometry().tiles_x, d.geometry().tiles_y);
                tile_sel_.set_rect(IRect{0, 0, d.geometry().tiles_x, d.geometry().tiles_y}, true);
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
                ctx.paragraph("Click a basin to flood it to the level -- the water runs as far as the ground holds it. Ctrl "
                              "drains the whole body you click.",
                              &dim);
                break;
            case TileTool::Select: break;
        }
    }

    draw_tile_selection_section_(ctx, *t);

    // --- Hovered tile ---
    if (ctx.collapsing_header("Tile Under Cursor", false, nullptr, I::Info)) {
        if (tile_hover_valid_ && d.contains(tile_hover_.x, tile_hover_.y)) {
            const TileCell& r = d.at(tile_hover_.x, tile_hover_.y);
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
            if (r.wet()) {
                const int level_steps = static_cast<int>(std::lround(r.water_level / std::max(1e-4f, hstep)));
                const char* kind = r.water_kind == static_cast<std::uint8_t>(WaterKind::River) ? "river"
                                  : r.water_kind == static_cast<std::uint8_t>(WaterKind::Ocean) ? "sea" : "lake";
                flags += std::string(kind) + " water to " + std::to_string(level_steps) + " (" +
                         std::to_string(level_steps - r.steps) + " deep) ";
            }
            if (r.water_kind == static_cast<std::uint8_t>(WaterKind::Ocean)) flags += "ocean ";
            if (r.has(TileCell::Coast)) flags += "coast ";
            if (r.has(TileCell::River)) flags += "river ";
            if (r.region != TileCell::k_no_id) flags += "region " + std::to_string(r.region) + " ";
            if (r.country != TileCell::k_no_id) flags += "country " + std::to_string(r.country);
            if (!flags.empty()) ctx.label(flags, &dim);
        } else {
            ctx.label("Hover the terrain.", &dim);
        }
    }

    // --- Generate ---
    if (ctx.collapsing_header("Generate", false, nullptr, I::World)) {
        ctx.paragraph("The tiles were generated by mapcoopa from the terrain's settings (the World Generator), then "
                      "edited. Regenerate replaces every tile with a fresh world from the current settings -- your edits "
                      "included. It can be undone.",
                      &dim);
        if (ctx.button("Open World Generator...", -1, !app_.playing(), I::World)) app_.world_generator_panel_->open_world_generator(tile_object_);
        if (ctx.button(t->baking() ? "Regenerating..." : "Regenerate World", -1, !t->baking() && !tile_regen_data_, I::Restart)) {
            tile_regenerate_();
        }
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d x %d tiles%s", d.geometry().tiles_x, d.geometry().tiles_y,
                 d.dirty_on_disk() ? "  --  unsaved (Ctrl S)" : "");
    ctx.label(buf, &dim);
}

void TileModePanel::draw_tile_selection_section_(imm::Context& ctx, TerrainComponent& t) {
    using I = imm::Icon;
    const glm::vec4 dim = ctx.style.text_dim;
    if (tile_sel_.empty() || t.field.empty()) return;
    if (!ctx.collapsing_header("Selected Tiles", true, nullptr, I::SelectBox)) return;
    const TerraData& d = *t.field.data();
    const float hstep = t.params.height_step;

    // What the selected tiles share: a value when they all agree, "mixed" when not.
    struct Common {
        bool first = true, mixed_steps = false, mixed_top = false, mixed_side = false, mixed_biome = false;
        bool mixed_region = false, mixed_country = false;
        int steps = 0, water = 0, water_level = 0;
        bool mixed_water = false;
        TileKind top{}, side{};
        int biome = 0, region = 0, country = 0;
        double temperature = 0.0, moisture = 0.0;
    } c;
    tile_sel_.for_each([&](int x, int y) {
        if (!d.contains(x, y)) return;
        const TileCell& r = d.at(x, y);
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
        if (r.wet()) {
            const int level_steps = static_cast<int>(std::lround(r.water_level / std::max(1e-4f, hstep)));
            if (c.water > 0 && level_steps != c.water_level) c.mixed_water = true;
            c.water_level = level_steps;
            ++c.water;
        }
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
        tile_edit_selection_("Set Tile Height", [steps, hstep](TileCell& r) { move_ground(r, steps, hstep); }, "tile_height", true);
    }
    ctx.tooltip("Height\nSets every selected tile's ground to this height (under water, the bed; the water keeps its level)");
    if (ctx.button("-1 Step", 90, true, I::ArrowDown)) tile_edit_selection_("Lower Tiles", [hstep](TileCell& r) {
        move_ground(r, r.steps - 1, hstep);
    }, {}, true);
    ctx.same_line();
    if (ctx.button("+1 Step", 90, true, I::Plus)) tile_edit_selection_("Raise Tiles", [hstep](TileCell& r) {
        move_ground(r, r.steps + 1, hstep);
    }, {}, true);

    // Surfaces.
    auto kind_combo = [&](const char* label, TileKind cur, bool mixed, bool top) {
        std::vector<std::string> opts = kind_options();
        int k = static_cast<int>(cur);
        if (mixed) { opts.push_back("(mixed)"); k = static_cast<int>(opts.size()) - 1; }
        const int was = k;
        if (ctx.combo(label, &k, opts) && k != was && k < static_cast<int>(toy::world::k_tile_kind_count)) {
            const TileKind kind = static_cast<TileKind>(k);
            tile_edit_selection_(top ? "Set Tile Surface" : "Set Tile Sides", [kind, top](TileCell& r) {
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
            tile_edit_selection_("Set Tile Biome", [b](TileCell& r) { r.biome = static_cast<std::uint8_t>(b); });
        }
    }
    float temp = static_cast<float>(c.temperature);
    if (ctx.slider_float("Temperature", &temp, 0.0f, 1.0f, "%.2f")) {
        tile_edit_selection_("Set Tile Temperature", [temp](TileCell& r) { r.temperature = TileCell::quantise01(temp); }, "tile_temperature");
    }
    ctx.tooltip("Temperature\nmapcoopa's yearly mean: 0 polar, 1 equatorial (the average is shown when tiles differ)");
    float moist = static_cast<float>(c.moisture);
    if (ctx.slider_float("Moisture", &moist, 0.0f, 1.0f, "%.2f")) {
        tile_edit_selection_("Set Tile Moisture", [moist](TileCell& r) { r.moisture = TileCell::quantise01(moist); }, "tile_moisture");
    }

    // Water: the terrace it fills to. Raising it over the ground floods; at or under it, dry.
    {
        int level = c.water > 0 ? c.water_level : c.steps + 1;
        const char* label = c.water == 0 ? "Water Level (dry)" : (c.water < n || c.mixed_water ? "Water Level (mixed)" : "Water Level");
        if (ctx.drag_int(label, &level, 0.1f, 0, 2000)) {
            tile_edit_selection_("Set Water Level", [level, hstep](TileCell& r) { set_water_level(r, level, hstep); },
                                 "tile_water", true);
        }
        ctx.tooltip("Water Level\nThe terrace the water fills to, in steps; every selected tile with lower ground is under "
                    "it, and it runs on into any lower ground beside them. Drag it under the ground to dry the tiles");
        if (c.water > 0 && ctx.button("Drain", 90, true, I::X)) {
            tile_edit_selection_("Drain Water", [hstep](TileCell& r) { set_water_level(r, r.steps, hstep); });
        }
    }

    // Region / country ids.
    int region = c.region == TileCell::k_no_id ? -1 : c.region;
    if (ctx.drag_int(c.mixed_region ? "Region (mixed)" : "Region", &region, 0.05f, -1, 65534)) {
        tile_edit_selection_("Set Tile Region", [region](TileCell& r) {
            r.region = region < 0 ? TileCell::k_no_id : static_cast<std::uint16_t>(region);
        }, "tile_region");
    }
    int country = c.country == TileCell::k_no_id ? -1 : c.country;
    if (ctx.drag_int(c.mixed_country ? "Country (mixed)" : "Country", &country, 0.05f, -1, 65534)) {
        tile_edit_selection_("Set Tile Country", [country](TileCell& r) {
            r.country = country < 0 ? TileCell::k_no_id : static_cast<std::uint16_t>(country);
        }, "tile_country");
    }
    // A drag's merged step closes when the mouse lets go.
    if (!ctx.input().down[0]) tile_undo_.end_merge();
}

} // namespace editor
} // namespace toy
