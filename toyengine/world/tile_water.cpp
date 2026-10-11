#include <toyengine/world/tile_water.h>

#include <algorithm>
#include <deque>

namespace toy {
namespace world {

namespace {

constexpr int k_dx[4] = {1, -1, 0, 0};
constexpr int k_dy[4] = {0, 0, 1, -1};

IRect clip_limit(const TileMapData& tiles, const IRect& limit) {
    const IRect whole{0, 0, tiles.width(), tiles.height()};
    if (limit.empty()) return whole;
    return IRect{std::max(0, limit.x0), std::max(0, limit.y0), std::min(whole.x1, limit.x1), std::min(whole.y1, limit.y1)};
}

bool inside(const IRect& r, int x, int y) { return x >= r.x0 && y >= r.y0 && x < r.x1 && y < r.y1; }

void grow(IRect& r, int x, int y) { r.include(IRect{x, y, x + 1, y + 1}); }

/**
 * @brief Spreads water outward from the queued tiles into dry neighbours whose ground lies under
 *        the water's level, each taking the water it came from. With `raise_water`, lower water
 *        is raised too (a flood); without, it is left alone -- a river steps down terraces, and
 *        each step is meant to be lower than the one above it.
 */
void spread(TileMapData& tiles, std::deque<glm::ivec2>& queue, const IRect& limit, std::size_t cap, bool raise_water,
            IRect& changed) {
    std::size_t count = 0;
    while (!queue.empty() && count < cap) {
        const glm::ivec2 p = queue.front();
        queue.pop_front();
        const TileRecord from = tiles.at(p.x, p.y);
        if (!from.water()) continue;
        const int level = from.water_level();
        for (int k = 0; k < 4; ++k) {
            const int nx = p.x + k_dx[k], ny = p.y + k_dy[k];
            if (!inside(limit, nx, ny)) continue;
            TileRecord& to = tiles.at(nx, ny);
            if (to.steps >= level || (to.water() && (!raise_water || to.water_level() >= level))) continue;
            // Water that runs on from a river is still that river's; anything else pools.
            to.set_water(level, from.has(TileRecord::Ocean), from.has(TileRecord::Flowing));
            to.flow = from.has(TileRecord::Flowing) ? from.flow : 0;
            grow(changed, nx, ny);
            queue.push_back({nx, ny});
            ++count;
        }
    }
}

} // namespace

IRect flood_water(TileMapData& tiles, const std::vector<glm::ivec2>& seeds, int level, bool ocean, std::size_t cap) {
    IRect changed;
    if (tiles.empty()) return changed;
    const IRect whole = clip_limit(tiles, IRect{});
    std::deque<glm::ivec2> queue;
    for (const glm::ivec2& s : seeds) {
        if (!inside(whole, s.x, s.y)) continue;
        TileRecord& r = tiles.at(s.x, s.y);
        if (r.steps >= level || (r.water() && r.water_level() >= level)) continue;
        r.set_water(level, ocean, false);
        grow(changed, s.x, s.y);
        queue.push_back(s);
    }
    spread(tiles, queue, whole, cap, true, changed);
    if (!changed.empty()) tiles.mark_dirty(changed);
    return changed;
}

IRect drain_water(TileMapData& tiles, const std::vector<glm::ivec2>& seeds, std::size_t cap) {
    IRect changed;
    if (tiles.empty()) return changed;
    const IRect whole = clip_limit(tiles, IRect{});
    std::deque<glm::ivec2> queue;
    std::size_t count = 0;
    auto take = [&](int x, int y) {
        TileRecord& r = tiles.at(x, y);
        if (!r.water()) return;
        r.set_water(r.steps);
        grow(changed, x, y);
        queue.push_back({x, y});
        ++count;
    };
    for (const glm::ivec2& s : seeds)
        if (inside(whole, s.x, s.y)) take(s.x, s.y);
    while (!queue.empty() && count < cap) {
        const glm::ivec2 p = queue.front();
        queue.pop_front();
        for (int k = 0; k < 4; ++k) {
            const int nx = p.x + k_dx[k], ny = p.y + k_dy[k];
            if (inside(whole, nx, ny)) take(nx, ny);
        }
    }
    if (!changed.empty()) tiles.mark_dirty(changed);
    return changed;
}

IRect settle_water(TileMapData& tiles, const IRect& rect, const IRect& limit, std::size_t cap) {
    IRect changed;
    if (tiles.empty()) return changed;
    const IRect bound = clip_limit(tiles, limit);
    const IRect r = clip_limit(tiles, IRect{rect.x0 - 1, rect.y0 - 1, rect.x1 + 1, rect.y1 + 1});
    std::deque<glm::ivec2> queue;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            TileRecord& t = tiles.at(x, y);
            if (!t.water()) continue;
            if (inside(rect, x, y) && t.steps >= t.water_level()) {
                t.set_water(t.steps);
                grow(changed, x, y);
                continue;
            }
            queue.push_back({x, y});
        }
    }
    // Ground that dropped beside water is flooded by it -- and on, as far as the basin runs.
    spread(tiles, queue, bound, cap, false, changed);
    if (!changed.empty()) tiles.mark_dirty(changed);
    return changed;
}

void settle_water_rounding(TileMapData& tiles, const IRect& rect) {
    const int w = tiles.width(), h = tiles.height();
    const int x0 = std::max(0, rect.x0), y0 = std::max(0, rect.y0);
    const int x1 = std::min(w, rect.x1), y1 = std::min(h, rect.y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            TileRecord& r = tiles.at(x, y);
            if (r.water()) {
                const int level = r.water_level();
                const bool ocean = r.has(TileRecord::Ocean), flowing = r.has(TileRecord::Flowing);
                const std::uint8_t flow = r.flow;
                if (r.steps > level - 1) r.steps = static_cast<std::int16_t>(level - 1);
                if (r.steps < level - 255) r.steps = static_cast<std::int16_t>(level - 255);
                r.set_water(level, ocean, flowing);
                r.flow = flowing ? flow : 0;
                continue;
            }
            int bank = r.steps;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const TileRecord& o = tiles.at(nx, ny);
                    if (o.water()) bank = std::max(bank, o.water_level());
                }
            }
            r.steps = static_cast<std::int16_t>(bank);
        }
    }
}

} // namespace world
} // namespace toy
