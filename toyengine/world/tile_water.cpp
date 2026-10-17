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
 * @brief The tile one step from `(x, y)` along `(dx, dy)`: the next column in storage on a flat
 *        world, and the tile that is really beside it on a planet, which may be on another face
 *        and turned (planet.h). Returns false when the step leaves the grid.
 */
bool step_to(const PlanetLayout* planet, const IRect& limit, int x, int y, int dx, int dy, glm::ivec2& out) {
    out = planet != nullptr ? planet->neighbour(x, y, dx, dy) : glm::ivec2{x + dx, y + dy};
    return inside(limit, out.x, out.y);
}

/**
 * @brief Spreads water outward from the queued tiles into dry neighbours whose ground lies under
 *        the water's level, each taking the water it came from. With `raise_water`, lower water
 *        is raised too (a flood); without, it is left alone -- a river steps down terraces, and
 *        each step is meant to be lower than the one above it.
 *
 * @param budget Tiles this call may fill; the queue is left where it got to, so a caller can
 *               come back for the rest (step_settle()).
 * @return How many tiles it filled.
 */
std::size_t spread(TileMapData& tiles, std::deque<glm::ivec2>& queue, const IRect& limit, std::size_t budget,
                   bool raise_water, IRect& changed, const PlanetLayout* planet = nullptr) {
    std::size_t count = 0;
    while (!queue.empty() && count < budget) {
        const glm::ivec2 p = queue.front();
        queue.pop_front();
        const TileRecord from = tiles.at(p.x, p.y);
        if (!from.water()) continue;
        const int level = from.water_level();
        for (int k = 0; k < 4; ++k) {
            glm::ivec2 n;
            if (!step_to(planet, limit, p.x, p.y, k_dx[k], k_dy[k], n)) continue;
            TileRecord& to = tiles.at(n.x, n.y);
            if (to.steps >= level || (to.water() && (!raise_water || to.water_level() >= level))) continue;
            // Water that runs on from a river is still that river's; anything else pools.
            to.set_water(level, from.has(TileRecord::Ocean), from.has(TileRecord::Flowing));
            to.flow = from.has(TileRecord::Flowing) ? from.flow : 0;
            grow(changed, n.x, n.y);
            queue.push_back({n.x, n.y});
            ++count;
        }
    }
    return count;
}

} // namespace

IRect flood_water(TileMapData& tiles, const std::vector<glm::ivec2>& seeds, int level, bool ocean, std::size_t cap,
                  const PlanetLayout* planet) {
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
    spread(tiles, queue, whole, cap, true, changed, planet);
    if (!changed.empty()) tiles.mark_dirty(changed);
    return changed;
}

IRect drain_water(TileMapData& tiles, const std::vector<glm::ivec2>& seeds, std::size_t cap,
                  const PlanetLayout* planet) {
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
            glm::ivec2 n;
            if (step_to(planet, whole, p.x, p.y, k_dx[k], k_dy[k], n)) take(n.x, n.y);
        }
    }
    if (!changed.empty()) tiles.mark_dirty(changed);
    return changed;
}

WaterSettle begin_settle(TileMapData& tiles, const IRect& rect, const IRect& limit,
                         const PlanetLayout* planet, std::size_t cap) {
    WaterSettle settle;
    if (planet != nullptr) {
        settle.layout = *planet;
        settle.on_planet = true;
    }
    settle.cap = cap;
    if (tiles.empty()) return settle;
    settle.limit = clip_limit(tiles, limit);
    IRect changed;
    // The ring around the edit as well as the edit itself: water just outside it is what floods
    // ground that dropped inside. On a planet the ring is taken through the layout, so an edit
    // against a face's edge is seeded from the face next door rather than from across the globe.
    const IRect r = clip_limit(tiles, IRect{rect.x0 - 1, rect.y0 - 1, rect.x1 + 1, rect.y1 + 1});
    const auto consider = [&](int x, int y) {
        TileRecord& t = tiles.at(x, y);
        if (!t.water()) return;
        if (inside(rect, x, y) && t.steps >= t.water_level()) {
            t.set_water(t.steps);          // its bed rose through the surface: dry land now
            grow(changed, x, y);
            return;
        }
        settle.frontier.push_back({x, y});
    };
    if (planet == nullptr) {
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x) consider(x, y);
    } else {
        const IRect in = clip_limit(tiles, rect);
        for (int y = in.y0; y < in.y1; ++y)
            for (int x = in.x0; x < in.x1; ++x) consider(x, y);
        for (int y = in.y0; y < in.y1; ++y) {
            for (int x = in.x0; x < in.x1; ++x) {
                for (int k = 0; k < 4; ++k) {
                    glm::ivec2 n;
                    if (!step_to(planet, settle.limit, x, y, k_dx[k], k_dy[k], n)) continue;
                    if (inside(in, n.x, n.y)) continue;           // already considered above
                    if (tiles.at(n.x, n.y).water()) settle.frontier.push_back(n);
                }
            }
        }
    }
    if (!changed.empty()) tiles.mark_dirty(changed);
    return settle;
}

IRect step_settle(TileMapData& tiles, WaterSettle& settle, std::size_t budget) {
    IRect changed;
    if (tiles.empty() || settle.done() || budget == 0) return changed;
    const std::size_t room = settle.cap - settle.spent;
    settle.spent += spread(tiles, settle.frontier, settle.limit, std::min(budget, room), false, changed,
                           settle.planet());
    if (!changed.empty()) tiles.mark_dirty(changed);
    return changed;
}

IRect settle_water(TileMapData& tiles, const IRect& rect, const IRect& limit, std::size_t cap,
                   const PlanetLayout* planet) {
    IRect changed;
    if (tiles.empty()) return changed;
    // The whole walk in one call: what an editor brush wants, and what begin/step do a frame at
    // a time for gameplay. The two must agree -- tile_map_test asserts they end up identical.
    const std::uint64_t before = tiles.revision();
    WaterSettle settle = begin_settle(tiles, rect, limit, planet, cap);
    while (!settle.done()) step_settle(tiles, settle, cap);
    // Both halves mark what they touch, so the journal already holds this call's whole footprint.
    return tiles.revision() == before ? changed : tiles.dirty_since(before);
}

void settle_water_rounding(TileMapData& tiles, const IRect& rect, const PlanetLayout* planet) {
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
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (planet != nullptr) {
                        const glm::ivec2 t = planet->neighbour(x, y, dx, dy);   // across a face's edge too
                        nx = t.x;
                        ny = t.y;
                    } else if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                        continue;
                    }
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
