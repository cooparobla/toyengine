#include <toyengine/world/terrain_water.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace toy {
namespace world {

namespace {

constexpr int k_dx[4] = {1, -1, 0, 0};
constexpr int k_dy[4] = {0, 0, 1, -1};

/**
 * @brief Slack on a level comparison, as a fraction of a `Tiles` field's step.
 *
 * A terrace arrives as `n * height_step` and comes back through a division, so an exact test
 * would occasionally read a tile as a hair above its own terrace. A smooth field needs no slack
 * at all, and gets a value far under anything it can resolve.
 */
float level_epsilon(const TerrainField& field) {
    return field.mode() == TerrainMode::Tiles ? field.geometry().height_step * 1e-3f : 1e-4f;
}

IRect clip_limit(const TerrainField& field, const IRect& limit) {
    const TileMapData* tiles = field.tiles();
    const IRect whole{0, 0, tiles ? tiles->width() : 0, tiles ? tiles->height() : 0};
    if (limit.empty()) return whole;
    return IRect{std::max(0, limit.x0), std::max(0, limit.y0), std::min(whole.x1, limit.x1),
                 std::min(whole.y1, limit.y1)};
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
std::size_t spread(TerrainField& field, std::deque<glm::ivec2>& queue, const IRect& limit, std::size_t budget,
                   bool raise_water, IRect& changed, const PlanetLayout* planet) {
    const float eps = level_epsilon(field);
    std::size_t count = 0;
    while (!queue.empty() && count < budget) {
        const glm::ivec2 p = queue.front();
        queue.pop_front();
        const WaterCell from = field.water_at(p);
        if (!from.wet()) continue;
        for (int k = 0; k < 4; ++k) {
            glm::ivec2 n;
            if (!step_to(planet, limit, p.x, p.y, k_dx[k], k_dy[k], n)) continue;
            if (field.ground_at_tile(n) >= from.level - eps) continue;
            const WaterCell to = field.water_at(n);
            if (to.wet() && (!raise_water || to.level >= from.level - eps)) continue;
            // Water that runs on from a river is still that river's; anything else pools.
            field.set_water(n, from);
            grow(changed, n.x, n.y);
            queue.push_back(n);
            ++count;
        }
    }
    return count;
}

} // namespace

IRect flood_water(TerrainField& field, const std::vector<glm::ivec2>& seeds, float level, WaterKind kind,
                  std::size_t cap, const PlanetLayout* planet) {
    IRect changed;
    if (field.empty() || kind == WaterKind::None) return changed;
    const float eps = level_epsilon(field);
    const IRect whole = clip_limit(field, IRect{});
    std::deque<glm::ivec2> queue;
    for (const glm::ivec2& s : seeds) {
        if (!inside(whole, s.x, s.y)) continue;
        if (field.ground_at_tile(s) >= level - eps) continue;
        const WaterCell had = field.water_at(s);
        if (had.wet() && had.level >= level - eps) continue;
        field.set_water(s, WaterCell{kind, level, 0});
        grow(changed, s.x, s.y);
        queue.push_back(s);
    }
    spread(field, queue, whole, cap, true, changed, planet);
    if (!changed.empty()) field.mark_tiles_dirty(changed);
    return changed;
}

IRect drain_water(TerrainField& field, const std::vector<glm::ivec2>& seeds, std::size_t cap,
                  const PlanetLayout* planet) {
    IRect changed;
    if (field.empty()) return changed;
    const IRect whole = clip_limit(field, IRect{});
    std::deque<glm::ivec2> queue;
    std::size_t count = 0;
    auto take = [&](int x, int y) {
        if (!field.water_at(glm::ivec2(x, y)).wet()) return;
        field.clear_water(glm::ivec2(x, y));
        grow(changed, x, y);
        queue.push_back({x, y});
        ++count;
    };
    for (const glm::ivec2& s : seeds) {
        if (inside(whole, s.x, s.y)) take(s.x, s.y);
    }
    while (!queue.empty() && count < cap) {
        const glm::ivec2 p = queue.front();
        queue.pop_front();
        for (int k = 0; k < 4; ++k) {
            glm::ivec2 n;
            if (step_to(planet, whole, p.x, p.y, k_dx[k], k_dy[k], n)) take(n.x, n.y);
        }
    }
    if (!changed.empty()) field.mark_tiles_dirty(changed);
    return changed;
}

WaterSettle begin_settle(TerrainField& field, const IRect& rect, const IRect& limit, const PlanetLayout* planet,
                         std::size_t cap) {
    WaterSettle settle;
    if (planet != nullptr) {
        settle.layout = *planet;
        settle.on_planet = true;
    }
    settle.cap = cap;
    if (field.empty()) return settle;
    settle.limit = clip_limit(field, limit);
    const float eps = level_epsilon(field);
    IRect changed;
    // The ring around the edit as well as the edit itself: water just outside it is what floods
    // ground that dropped inside. On a planet the ring is taken through the layout, so an edit
    // against a face's edge is seeded from the face next door rather than from across the globe.
    const IRect r = clip_limit(field, IRect{rect.x0 - 1, rect.y0 - 1, rect.x1 + 1, rect.y1 + 1});
    const auto consider = [&](int x, int y) {
        const glm::ivec2 t(x, y);
        const WaterCell w = field.water_at(t);
        if (!w.wet()) return;
        if (inside(rect, x, y) && field.ground_at_tile(t) >= w.level - eps) {
            field.clear_water(t);          // its bed rose through the surface: dry land now
            grow(changed, x, y);
            return;
        }
        settle.frontier.push_back(t);
    };
    if (planet == nullptr) {
        for (int y = r.y0; y < r.y1; ++y) {
            for (int x = r.x0; x < r.x1; ++x) consider(x, y);
        }
    } else {
        const IRect in = clip_limit(field, rect);
        for (int y = in.y0; y < in.y1; ++y) {
            for (int x = in.x0; x < in.x1; ++x) consider(x, y);
        }
        for (int y = in.y0; y < in.y1; ++y) {
            for (int x = in.x0; x < in.x1; ++x) {
                for (int k = 0; k < 4; ++k) {
                    glm::ivec2 n;
                    if (!step_to(planet, settle.limit, x, y, k_dx[k], k_dy[k], n)) continue;
                    if (inside(in, n.x, n.y)) continue;           // already considered above
                    if (field.water_at(n).wet()) settle.frontier.push_back(n);
                }
            }
        }
    }
    if (!changed.empty()) field.mark_tiles_dirty(changed);
    return settle;
}

IRect step_settle(TerrainField& field, WaterSettle& settle, std::size_t budget) {
    IRect changed;
    if (field.empty() || settle.done() || budget == 0) return changed;
    const std::size_t room = settle.cap - settle.spent;
    settle.spent +=
        spread(field, settle.frontier, settle.limit, std::min(budget, room), false, changed, settle.planet());
    if (!changed.empty()) field.mark_tiles_dirty(changed);
    return changed;
}

IRect settle_water(TerrainField& field, const IRect& rect, const IRect& limit, std::size_t cap,
                   const PlanetLayout* planet) {
    IRect changed;
    if (field.empty()) return changed;
    // The whole walk in one call: what an editor brush wants, and what begin/step do a frame at
    // a time for gameplay. The two must agree -- terrain_water_test asserts they end up identical.
    const TerrainFieldCursor before = field.cursor();
    WaterSettle settle = begin_settle(field, rect, limit, planet, cap);
    while (!settle.done()) step_settle(field, settle, cap);
    // Both halves mark what they touch, so the journal already holds this call's whole footprint.
    return field.cursor() == before ? changed : field.dirty_since(before);
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
