#include <toyengine/world/tile_edit.h>

#include <toyengine/world/terrain_field.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include <toyengine/world/tile_water.h>

namespace toy {
namespace world {

namespace {

/// Applies `fn` to every record of `rect`, marking and returning the rect when anything changed
/// (fn returns true for a change).
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

/// Moves a tile's ground to `steps`, carrying its water up or down with it (the SURFACE stays
/// where it was -- the bed is what moved). Returns whether anything changed.
bool move_ground(TileRecord& r, std::int16_t steps) {
    if (steps == r.steps) return false;
    const int level = r.water_level();
    const bool water = r.water(), ocean = r.has(TileRecord::Ocean), flowing = r.has(TileRecord::Flowing);
    const std::uint8_t flow = r.flow;
    r.steps = steps;
    if (water) {
        r.set_water(level, ocean, flowing);
        if (flowing) r.flow = flow;
    }
    return true;
}

} // namespace

IRect tile_raise(TileMapData& d, const IRect& rect, int delta, bool settle, const PlanetLayout* planet) {
    // The ground moves -- under water, the bed -- and the water re-settles to it: a bed raised
    // through its surface leaves land, ground lowered beside water floods.
    IRect changed = for_tiles(d, rect, [delta](TileRecord& r, int, int) {
        return move_ground(r, clamp_steps(r.steps + delta));
    });
    if (settle && !changed.empty()) {
        TerrainField field = borrow_tiles(d);
        changed.include(settle_water(field, changed, IRect{}, k_water_flood_cap, planet));
    }
    return changed;
}

IRect tile_set_height(TileMapData& d, const IRect& rect, int steps, bool settle, const PlanetLayout* planet) {
    IRect changed = for_tiles(d, rect, [steps](TileRecord& r, int, int) {
        return move_ground(r, clamp_steps(steps));
    });
    if (settle && !changed.empty()) {
        TerrainField field = borrow_tiles(d);
        changed.include(settle_water(field, changed, IRect{}, k_water_flood_cap, planet));
    }
    return changed;
}

IRect tile_paint(TileMapData& d, const IRect& rect, TileKind kind, bool side) {
    const TileKind side_kind = side_kind_for(kind);
    // Under water too: a bed is ground like any other.
    return for_tiles(d, rect, [kind, side, side_kind](TileRecord& r, int, int) {
        const TileRecord before = r;
        r.set_top_kind(kind);
        if (side) r.set_side_kind(side_kind);
        return r != before;
    });
}

IRect tile_smooth(TileMapData& d, const IRect& rect, bool settle, const PlanetLayout* planet) {
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
    IRect changed = for_tiles(d, r, [&](TileRecord& t, int, int) { return move_ground(t, target[i++]); });
    if (settle && !changed.empty()) {
        TerrainField field = borrow_tiles(d);
        changed.include(settle_water(field, changed, IRect{}, k_water_flood_cap, planet));
    }
    return changed;
}

IRect tile_water(TileMapData& d, const IRect& rect, int level, bool fill, const PlanetLayout* planet) {
    const IRect r = rect.clipped(d.width(), d.height());
    std::vector<glm::ivec2> seeds;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) seeds.push_back({x, y});
    // Fill floods the basin the brush is in, to `level`, as far as the ground holds it; drain
    // takes away the whole body the brush touches.
    TerrainField field = borrow_tiles(d);
    // A terrace in steps and a level in metres are the same number over a borrowed grid.
    return fill ? flood_water(field, seeds, static_cast<float>(level), WaterKind::Lake, k_water_flood_cap, planet)
                : drain_water(field, seeds, k_water_flood_cap, planet);
}

} // namespace world
} // namespace toy
