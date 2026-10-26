#include "editor/terrain/tile_brush.h"

#include <toyengine/world/tile_edit.h>
#include <toyengine/world/tile_water.h>

#include <algorithm>
#include <array>
#include <deque>

#include <toyengine/world/terrain_params.h>

namespace toy::editor {

using toy::world::IRect;
using toy::world::TileKind;
using toy::world::TileMapData;
using toy::world::TileRecord;

const char* tile_tool_name(TileTool t) {
    switch (t) {
        case TileTool::Select:    return "Select";
        case TileTool::Raise:     return "Raise / Lower";
        case TileTool::Flatten:   return "Flatten";
        case TileTool::SetHeight: return "Set Height";
        case TileTool::Paint:     return "Paint";
        case TileTool::Smooth:    return "Smooth";
        case TileTool::Water:     return "Water";
    }
    return "?";
}

IRect tile_brush_rect(const glm::ivec2& tile, int size) {
    const int s = std::max(1, size);
    const int lo = (s - 1) / 2;
    return IRect{tile.x - lo, tile.y - lo, tile.x - lo + s, tile.y - lo + s};
}

// The ground tools themselves live in the engine (toyengine/world/tile_edit.h) so gameplay can
// edit terrain too; what stays here is the editor's half -- brush rects, selections and undo.
IRect tile_raise(TileMapData& d, const IRect& rect, int delta) {
    return toy::world::tile_raise(d, rect, delta);
}

IRect tile_set_height(TileMapData& d, const IRect& rect, int steps) {
    return toy::world::tile_set_height(d, rect, steps);
}

IRect tile_paint(TileMapData& d, const IRect& rect, TileKind kind, bool side) {
    return toy::world::tile_paint(d, rect, kind, side);
}

IRect tile_smooth(TileMapData& d, const IRect& rect) {
    return toy::world::tile_smooth(d, rect);
}

IRect tile_water(TileMapData& d, const IRect& rect, int level, bool fill) {
    return toy::world::tile_water(d, rect, level, fill);
}

// --- selection ---

void TileSelection::reset(int w, int h) {
    w_ = std::max(0, w);
    h_ = std::max(0, h);
    mask_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0);
    count_ = 0;
}

void TileSelection::clear() {
    std::fill(mask_.begin(), mask_.end(), 0);
    count_ = 0;
}

void TileSelection::set(int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    std::uint8_t& m = mask_[static_cast<std::size_t>(y * w_ + x)];
    if ((m != 0) == on) return;
    m = on ? 1 : 0;
    count_ += on ? 1 : -1;
}

void TileSelection::set_rect(const IRect& rect, bool on) {
    const IRect r = rect.clipped(w_, h_);
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) set(x, y, on);
}

IRect TileSelection::bounds() const {
    IRect b;
    for_each([&b](int x, int y) { b.include(IRect{x, y, x + 1, y + 1}); });
    return b;
}

void TileSelection::for_each(const std::function<void(int, int)>& fn) const {
    if (count_ == 0) return;
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x)
            if (mask_[static_cast<std::size_t>(y * w_ + x)]) fn(x, y);
}

void tile_select_similar(const TileMapData& d, const glm::ivec2& start, bool by_kind, TileSelection& sel) {
    if (!d.contains(start.x, start.y)) return;
    if (sel.width() != d.width() || sel.height() != d.height()) sel.reset(d.width(), d.height());
    const TileRecord& seed = d.at(start.x, start.y);
    auto matches = [&](const TileRecord& r) { return by_kind ? r.top_kind() == seed.top_kind() : r.steps == seed.steps; };
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(d.width()) * static_cast<std::size_t>(d.height()), 0);
    std::deque<glm::ivec2> open{start};
    seen[static_cast<std::size_t>(start.y * d.width() + start.x)] = 1;
    while (!open.empty()) {
        const glm::ivec2 p = open.front();
        open.pop_front();
        sel.set(p.x, p.y, true);
        for (const glm::ivec2 step : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)}) {
            const glm::ivec2 q = p + step;
            if (!d.contains(q.x, q.y)) continue;
            std::uint8_t& s = seen[static_cast<std::size_t>(q.y * d.width() + q.x)];
            if (s || !matches(d.at(q.x, q.y))) continue;
            s = 1;
            open.push_back(q);
        }
    }
}

IRect tile_edit_selection(TileMapData& d, const TileSelection& sel, const std::function<void(TileRecord&)>& edit) {
    IRect changed;
    sel.for_each([&](int x, int y) {
        if (!d.contains(x, y)) return;
        TileRecord& r = d.at(x, y);
        const TileRecord before = r;
        edit(r);
        if (r != before) changed.include(IRect{x, y, x + 1, y + 1});
    });
    if (!changed.empty()) d.mark_dirty(changed);
    return changed;
}

// --- undo ---

TilePatch capture_tile_patch(const std::shared_ptr<TileMapData>& d, const IRect& rect) {
    TilePatch p;
    if (!d) return p;
    p.data = d;
    p.uid = d->uid();
    p.rect = rect.clipped(d->width(), d->height());
    p.records = d->capture(p.rect);
    return p;
}

bool apply_tile_patch(const TilePatch& patch) {
    auto d = patch.data.lock();
    if (!d || d->uid() != patch.uid || patch.rect.empty()) return false;
    if (patch.width > 0 && (d->width() != patch.width || d->height() != patch.height)) d->resize(patch.width, patch.height);
    if (patch.rect.x1 > d->width() || patch.rect.y1 > d->height()) return false;
    d->restore(patch.rect, patch.records);
    return true;
}

} // namespace toy::editor
