#include <toyengine/world/tile_edit.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include <toyengine/world/terrain_water.h>

namespace toy {
namespace world {

namespace {

/// Applies `fn` to every cell of `rect`, marking and returning the rect when anything changed
/// (fn returns true for a change).
IRect for_tiles(TerraData& d, const IRect& rect, const std::function<bool(TileCell&, int, int)>& fn) {
    const IRect r = rect.clipped(d.geometry().tiles_x, d.geometry().tiles_y);
    if (r.empty()) return IRect{};
    bool changed = false;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) changed = fn(d.at(x, y), x, y) || changed;
    if (!changed) return IRect{};
    d.mark_tiles_dirty(r);
    return r;
}

std::int16_t clamp_steps(int v) { return static_cast<std::int16_t>(std::clamp(v, 0, 32000)); }

/// Moves a tile's ground to `steps`. The water's surface level never moves with it (it is stored
/// absolute, not as a depth), but a bed that rose through it drains the tile. Returns whether
/// anything changed.
bool move_ground(TileCell& c, std::int16_t steps, float height_step) {
    if (steps == c.steps) return false;
    c.steps = steps;
    if (c.wet() && c.water_level <= static_cast<float>(steps) * height_step) {
        c.water_kind = static_cast<std::uint8_t>(WaterKind::None);
        c.water_level = 0.0f;
        c.flow = 0;
        c.flow_authored = 0;
    }
    return true;
}

} // namespace

IRect tile_raise(TerrainField& field, const IRect& rect, int delta, bool settle, const PlanetLayout* planet) {
    TerraData* d = field.data();
    if (!d) return IRect{};
    const float hstep = d->geometry().height_step;
    // The ground moves -- under water, the bed -- and the water re-settles to it: a bed raised
    // through its surface leaves land, ground lowered beside water floods.
    IRect changed = for_tiles(*d, rect, [delta, hstep](TileCell& c, int, int) {
        return move_ground(c, clamp_steps(c.steps + delta), hstep);
    });
    if (settle && !changed.empty()) changed.include(settle_water(field, changed, IRect{}, k_water_flood_cap, planet));
    return changed;
}

IRect tile_set_height(TerrainField& field, const IRect& rect, int steps, bool settle, const PlanetLayout* planet) {
    TerraData* d = field.data();
    if (!d) return IRect{};
    const float hstep = d->geometry().height_step;
    IRect changed = for_tiles(*d, rect, [steps, hstep](TileCell& c, int, int) {
        return move_ground(c, clamp_steps(steps), hstep);
    });
    if (settle && !changed.empty()) changed.include(settle_water(field, changed, IRect{}, k_water_flood_cap, planet));
    return changed;
}

IRect tile_paint(TerrainField& field, const IRect& rect, TileKind kind, bool side) {
    TerraData* d = field.data();
    if (!d) return IRect{};
    const TileKind side_kind = side_kind_for(kind);
    // Under water too: a bed is ground like any other.
    return for_tiles(*d, rect, [kind, side, side_kind](TileCell& c, int, int) {
        const TileCell before = c;
        c.set_top_kind(kind);
        if (side) c.set_side_kind(side_kind);
        return c != before;
    });
}

IRect tile_smooth(TerrainField& field, const IRect& rect, bool settle, const PlanetLayout* planet) {
    TerraData* d = field.data();
    if (!d) return IRect{};
    const IRect r = rect.clipped(d->geometry().tiles_x, d->geometry().tiles_y);
    if (r.empty()) return IRect{};
    const float hstep = d->geometry().height_step;
    // Read every neighbourhood before writing any, so the result does not depend on walk order.
    std::vector<std::int16_t> target;
    target.reserve(static_cast<std::size_t>(r.width() * r.height()));
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            std::array<int, 9> n{};
            int k = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) n[static_cast<std::size_t>(k++)] = d->get(x + dx, y + dy).steps;
            std::nth_element(n.begin(), n.begin() + 4, n.end());
            const int cur = d->at(x, y).steps, med = n[4];
            target.push_back(static_cast<std::int16_t>(cur + (med > cur ? 1 : med < cur ? -1 : 0)));
        }
    }
    std::size_t i = 0;
    IRect changed = for_tiles(*d, r, [&](TileCell& t, int, int) { return move_ground(t, target[i++], hstep); });
    if (settle && !changed.empty()) changed.include(settle_water(field, changed, IRect{}, k_water_flood_cap, planet));
    return changed;
}

IRect tile_water(TerrainField& field, const IRect& rect, int level, bool fill, const PlanetLayout* planet) {
    TerraData* d = field.data();
    if (!d) return IRect{};
    const IRect r = rect.clipped(d->geometry().tiles_x, d->geometry().tiles_y);
    std::vector<glm::ivec2> seeds;
    for (int y = r.y0; y < r.y1; ++y)
        for (int x = r.x0; x < r.x1; ++x) seeds.push_back({x, y});
    // Fill floods the basin the brush is in, to `level`, as far as the ground holds it; drain
    // takes away the whole body the brush touches.
    const float hstep = d->geometry().height_step;
    return fill ? flood_water(field, seeds, static_cast<float>(level) * hstep, WaterKind::Lake, k_water_flood_cap, planet)
                : drain_water(field, seeds, k_water_flood_cap, planet);
}

} // namespace world
} // namespace toy
