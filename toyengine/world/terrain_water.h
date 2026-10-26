/**
 * @file terrain_water.h
 * @brief Editing the water over a terrain's tiles: flooding a basin, draining a body, and keeping
 *        the water consistent with the ground after an edit moves it -- in either terrain mode.
 *
 * Water is a **surface over the ground**, never geometry in it: a tile carries a kind
 * (None / Ocean / Lake / River), the height its surface stands at, and a river's downstream
 * octant (WaterCell in terrain_field.h). Depth is `level - ground`, so these operations read the
 * same whether the ground under them is a terrace or a smooth slope -- which is what lets one set
 * of them serve both modes.
 *
 * Levels are **metres** here. A `Tiles` field quantises them to whole terraces on the way in, so
 * a terrace `n` is the level `n * height_step` and every comparison below works out to the same
 * integer test it has always been.
 *
 * They all keep the one invariant: **water never stands above dry ground beside it.** Each
 * returns the rect of tiles it changed (possibly past the one it was given -- a flood runs as far
 * as its basin does) and marks it dirty, so the caller can capture undo and the chunks re-mesh.
 *
 * **Planets.** A planet's tiles are six cube faces side by side in one grid (planet.h), so
 * stepping one tile in storage walks off a face's edge into a column nowhere near it. Every
 * operation therefore takes an optional PlanetLayout and steps its neighbours through
 * `PlanetLayout::neighbour()` when it has one. Pass `params.planet ? &layout : nullptr`.
 *
 * **Budget.** settle_water() runs the whole flood in one call, which is what an editor brush
 * wants and a game frame does not: a hole dug beside the sea floods as far as the basin runs.
 * WaterSettle / begin_settle() / step_settle() are the same walk, resumable, so a caller can
 * spend a bounded number of tiles per frame on it (see TerrainComponent::water_settle_budget).
 */

#ifndef TOYENGINE_WORLD_TERRAIN_WATER_H
#define TOYENGINE_WORLD_TERRAIN_WATER_H

#include <cstddef>
#include <deque>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/planet.h>
#include <toyengine/world/terrain_field.h>

namespace toy {
namespace world {

/** @brief Default bound on how many tiles one flood or drain may change. */
inline constexpr std::size_t k_water_flood_cap = std::size_t{1} << 18;

/**
 * @brief Fills the basin under `level` from `seeds`: every tile 4-connected to a seed whose
 *        ground lies under `level`, and whose water (if any) stands lower, takes water to
 *        `level` as `kind`. Stops after `cap` tiles.
 */
IRect flood_water(TerrainField& field, const std::vector<glm::ivec2>& seeds, float level, WaterKind kind,
                  std::size_t cap = k_water_flood_cap, const PlanetLayout* planet = nullptr);

/** @brief Removes the water 4-connected to `seeds` (the whole body, at any level). Stops after `cap` tiles. */
IRect drain_water(TerrainField& field, const std::vector<glm::ivec2>& seeds, std::size_t cap = k_water_flood_cap,
                  const PlanetLayout* planet = nullptr);

/**
 * @brief Re-settles water after the ground in `rect` moved: water whose ground rose through its
 *        surface is gone, and water beside ground that dropped below it spreads in (as far as
 *        the new basin runs, within `limit` and `cap`). The editor's ground brushes call this.
 *
 * @param limit Spreading never leaves this rect (clip it to keep an undo patch bounded); an
 *              empty rect means the whole grid.
 */
IRect settle_water(TerrainField& field, const IRect& rect, const IRect& limit = IRect{},
                   std::size_t cap = k_water_flood_cap, const PlanetLayout* planet = nullptr);

/**
 * @struct WaterSettle
 * @brief A settle in progress: the tiles whose neighbours have still to be looked at, and where
 *        the spread is allowed to go.
 *
 * Carrying the frontier rather than re-deriving it is what makes the walk resumable: each
 * step_settle() pops some of it, floods what those tiles reach, and pushes the tiles it filled.
 * An empty frontier means the water has finished moving.
 */
struct WaterSettle {
    std::deque<glm::ivec2> frontier;      /**< @brief Water tiles whose dry neighbours are still to be tried. */
    IRect                  limit;         /**< @brief The spread never leaves this rect. */
    /**
     * @brief The cube-sphere, when `on_planet` -- held BY VALUE, not borrowed.
     *
     * A settle outlives the call that started it (that is the point of it), and a pointer to a
     * layout built on the caller's stack would be dangling by the next frame.
     */
    PlanetLayout           layout;
    bool                   on_planet = false;
    std::size_t            spent = 0;     /**< @brief Tiles filled so far, against `cap`. */
    std::size_t            cap = k_water_flood_cap; /**< @brief Total bound over the whole settle. */

    /** @brief Nothing left to do -- the water has reached its new shape. */
    bool done() const { return frontier.empty() || spent >= cap; }
    /** @brief The layout to step neighbours through, or null on a flat world. */
    const PlanetLayout* planet() const { return on_planet ? &layout : nullptr; }
};

/**
 * @brief Starts a settle: applies the part that is immediate -- water inside `rect` whose ground
 *        rose through its surface is gone -- and seeds the frontier with the water around it.
 *
 * Marks what it removed dirty. The spreading half is step_settle()'s.
 *
 * @param rect   The tiles whose ground just moved.
 * @param limit  Spreading never leaves this rect; an empty rect means the whole grid.
 * @param planet The cube-sphere, on a planet; null on a flat world.
 * @param cap    Bound on the tiles the whole settle may fill.
 */
WaterSettle begin_settle(TerrainField& field, const IRect& rect, const IRect& limit = IRect{},
                         const PlanetLayout* planet = nullptr, std::size_t cap = k_water_flood_cap);

/**
 * @brief Spreads at most `budget` tiles' worth of `settle`, and marks what it filled as ONE rect
 *        -- a flood crossing a frame at a time would otherwise journal (and re-stale chunks for)
 *        every tile separately.
 *
 * @return The rect filled this step, empty when nothing moved.
 */
IRect step_settle(TerrainField& field, WaterSettle& settle, std::size_t budget);

/**
 * @brief The bake's rounding rule over `rect` (see the map bake): a dry tile beside water rounds
 *        **up** to that water's terrace -- it is the bank -- and a flooded tile's bed rounds
 *        **down** to at least one step under its surface. Does not mark dirty: the bake assigns
 *        the whole grid anyway.
 *
 * `Tiles` only, and deliberately. It exists because a terraced world has to choose which way a
 * fractional shoreline rounds, and choosing the ground's way is what keeps water off dry land. A
 * smooth field has no such choice to make: its shoreline is simply where the ground crosses the
 * water's level, so the terrain's own slope is the beach.
 */
void settle_water_rounding(TileMapData& tiles, const IRect& rect, const PlanetLayout* planet = nullptr);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_WATER_H
