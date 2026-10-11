#include "editor/terrain/tile_brush.h"

#include <toyengine/world/tile_water.h>

#include <algorithm>
#include <array>
#include <deque>

#include <toyengine/world/terrain_sampler.h>

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

namespace {

/// Visits every tile of `rect` clipped to the grid; returns the clipped rect, marked dirty when
/// anything changed (fn returns true for a change).
IRect for_tiles(TileMapData& d, const IRect& rect, const std::function<bool(TileRecord&, int, int)>& fn) {
    const IRect r = rect.clipped(d.width(), d.height());
    if (r.empty()) return IRect{};
    bool changed = false;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) changed = fn(d.at(x, y), x, y) || changed;
    if (!changed) return IRect{};
    d.mark_dirty(r);
    return r;
}

std::int16_t clamp_steps(int v) { return static_cast<std::int16_t>(std::clamp(v, 0, 32000)); }

} // namespace

IRect tile_raise(TileMapData& d, const IRect& rect, int delta) {
    // The ground moves -- under water, the bed -- and the water re-settles to it: a bed raised
    // through its surface leaves land, ground lowered beside water floods.
    IRect changed = for_tiles(d, rect, [delta](TileRecord& r, int, int) {
        const int level = r.water_level();
        const bool water = r.water(), ocean = r.has(TileRecord::Ocean), flowing = r.has(TileRecord::Flowing);
        const std::uint8_t flow = r.flow;
        const std::int16_t before = r.steps;
        r.steps = clamp_steps(r.steps + delta);
        if (water) {
            r.set_water(level, ocean, flowing);
            if (flowing) r.flow = flow;
        }
        return r.steps != before;
    });
    if (!changed.empty()) changed.include(toy::world::settle_water(d, changed));
    return changed;
}

IRect tile_set_height(TileMapData& d, const IRect& rect, int steps) {
    IRect changed = for_tiles(d, rect, [steps](TileRecord& r, int, int) {
        const std::int16_t v = clamp_steps(steps);
        if (v == r.steps) return false;
        const int level = r.water_level();
        const bool water = r.water(), ocean = r.has(TileRecord::Ocean), flowing = r.has(TileRecord::Flowing);
        const std::uint8_t flow = r.flow;
        r.steps = v;
        if (water) {
            r.set_water(level, ocean, flowing);
            if (flowing) r.flow = flow;
        }
        return true;
    });
    if (!changed.empty()) changed.include(toy::world::settle_water(d, changed));
    return changed;
}

IRect tile_paint(TileMapData& d, const IRect& rect, TileKind kind, bool side) {
    const TileKind side_kind = toy::world::TerrainSampler::side_kind_for(kind);
    // Under water too: a bed is ground like any other.
    return for_tiles(d, rect, [kind, side, side_kind](TileRecord& r, int, int) {
        const TileRecord before = r;
        r.set_top_kind(kind);
        if (side) r.set_side_kind(side_kind);
        return r != before;
    });
}

IRect tile_smooth(TileMapData& d, const IRect& rect) {
    const IRect r = rect.clipped(d.width(), d.height());
    if (r.empty()) return IRect{};
    // Read every neighbourhood before writing any, so the result does not depend on walk order.
    std::vector<std::int16_t> target;
    target.reserve(static_cast<std::size_t>(r.width() * r.height()));
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            std::array<int, 9> n{};
            int k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) n[static_cast<std::size_t>(k++)] = d.get(x + dx, y + dy).steps;
            std::nth_element(n.begin(), n.begin() + 4, n.end());
            const int cur = d.at(x, y).steps, med = n[4];
            target.push_back(static_cast<std::int16_t>(cur + (med > cur ? 1 : med < cur ? -1 : 0)));
        }
    }
    std::size_t i = 0;
    IRect changed = for_tiles(d, r, [&](TileRecord& t, int, int) {
        const std::int16_t v = target[i++];
        if (v == t.steps) return false;
        const int level = t.water_level();
        const bool water = t.water(), ocean = t.has(TileRecord::Ocean), flowing = t.has(TileRecord::Flowing);
        const std::uint8_t flow = t.flow;
        t.steps = v;
        if (water) {
            t.set_water(level, ocean, flowing);
            if (flowing) t.flow = flow;
        }
        return true;
    });
    if (!changed.empty()) changed.include(toy::world::settle_water(d, changed));
    return changed;
}

IRect tile_water(TileMapData& d, const IRect& rect, int level, bool fill) {
    const IRect r = rect.clipped(d.width(), d.height());
    std::vector<glm::ivec2> seeds;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) seeds.push_back({x, y});
    // Fill floods the basin the brush is in, to `level`, as far as the ground holds it; drain
    // takes away the whole body the brush touches.
    return fill ? toy::world::flood_water(d, seeds, level) : toy::world::drain_water(d, seeds);
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
    if (patch.rect.x1 > d->width() || patch.rect.y1 > d->height()) return false;
    d->restore(patch.rect, patch.records);
    return true;
}

} // namespace toy::editor
