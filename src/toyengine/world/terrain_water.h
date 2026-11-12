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
 * a terrace `n` is the level `n * height_step` and every comparison below works out to an
 * integer test.
 *
 * They all keep the one invariant: **water never stands above dry ground beside it.** Each
 * returns the rect of tiles it changed (possibly past the one it was given -- a flood runs as far
 * as its basin does) and marks it dirty, so the caller can capture undo and the chunks re-mesh.
 *
 * **Other layouts.** Where a terrain's tiles are not one flat grid (world/tile_grid.h), so
 * stepping one tile in storage walks off a face's edge into a column nowhere near it. Every
 * operation therefore takes an optional TileGrid and steps its neighbours through
 * `TileGrid::neighbour()` when it has one. Pass `terrain.topology.get()`; null is the flat
 * grid, where a neighbour is `(x + dx, y + dy)`.
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

#include <memory>

#include <toyengine/world/tile_grid.h>
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
                  std::size_t cap = k_water_flood_cap, const TileGrid* topology = nullptr);

/** @brief Removes the water 4-connected to `seeds` (the whole body, at any level). Stops after `cap` tiles. */
IRect drain_water(TerrainField& field, const std::vector<glm::ivec2>& seeds, std::size_t cap = k_water_flood_cap,
                  const TileGrid* topology = nullptr);

/**
 * @brief Re-settles water after the ground in `rect` moved: water whose ground rose through its
 *        surface is gone, and water beside ground that dropped below it spreads in (as far as
 *        the new basin runs, within `limit` and `cap`). The editor's ground brushes call this.
 *
 * @param limit Spreading never leaves this rect (clip it to keep an undo patch bounded); an
 *              empty rect means the whole grid.
 */
IRect settle_water(TerrainField& field, const IRect& rect, const IRect& limit = IRect{},
                   std::size_t cap = k_water_flood_cap, const TileGrid* topology = nullptr);

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
     * @brief The tile layout to step neighbours through; null on a flat grid.
     *
     * SHARED, not borrowed: a settle outlives the call that started it -- that is the point of
     * it -- and it may also outlive the TerrainComponent whose layout this is, so holding a
     * share keeps it alive for as long as the settle needs it.
     */
    std::shared_ptr<const TileGrid> topology;
    std::size_t            spent = 0;     /**< @brief Tiles filled so far, against `cap`. */
    std::size_t            cap = k_water_flood_cap; /**< @brief Total bound over the whole settle. */

    /** @brief Nothing left to do -- the water has reached its new shape. */
    bool done() const { return frontier.empty() || spent >= cap; }
    /** @brief The layout to step neighbours through, or null on a flat grid. */
    const TileGrid* layout() const { return topology.get(); }
};

/**
 * @brief Starts a settle: applies the part that is immediate -- water inside `rect` whose ground
 *        rose through its surface is gone -- and seeds the frontier with the water around it.
 *
 * Marks what it removed dirty. The spreading half is step_settle()'s.
 *
 * @param rect   The tiles whose ground just moved.
 * @param limit  Spreading never leaves this rect; an empty rect means the whole grid.
 * @param topology The tile layout; null on a flat grid. Taken by share, not borrowed: the
 *        settle outlives this call and may outlive the terrain that owns the layout.
 * @param cap    Bound on the tiles the whole settle may fill.
 */
WaterSettle begin_settle(TerrainField& field, const IRect& rect, const IRect& limit = IRect{},
                         std::shared_ptr<const TileGrid> topology = {},
                         std::size_t cap = k_water_flood_cap);

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
 * For a field with no height plane only, and deliberately. It exists because a terraced world has
 * to choose which way a fractional shoreline rounds, and choosing the ground's way is what keeps
 * water off dry land. A smooth field has no such choice to make: its shoreline is simply where the
 * ground crosses the water's level, so the terrain's own slope is the beach -- `grade_water_shore()`,
 * below, is what keeps that true.
 */
void settle_water_rounding(TerrainField& field, const IRect& rect, const TileGrid* topology = nullptr);

/** @brief How the ground is shaped under and around water on a SMOOTH (Heightmap) field. */
struct ShoreGrade {
    /** @brief Metres a wet tile's bed is kept under its own water level. */
    float clearance = 0.03f;
    /** @brief Extra metres the bed reaches `band` tiles in from the shoreline; 0 keeps the bed as given. */
    float depth = 0.0f;
    /** @brief Tiles the extra depth in `depth` ramps over. */
    float band = 4.0f;
};

/**
 * @brief Shapes a raw height plane so a smooth terrain's shoreline is a beach, not a tile-stepped
 *        trench. `settle_water_rounding()`'s opposite number: a terraced world only has to choose
 *        which way a fractional shoreline rounds, where a smooth one has to make sure the bed
 *        genuinely falls away from the waterline, because its shoreline is wherever the ground
 *        crosses the level and the water shader's edge fade and foam band are measured in metres
 *        of depth there.
 *
 * Three independent passes, each a plain ceiling on a height sample -- so running it twice changes
 * nothing the second time, and it can never raise a sample, so it can never flood land no tile in
 * `water` called wet:
 *   1. Lowers a wet tile's whole corner block uniformly until the value at its exact CENTRE sits
 *      `clearance` under its level -- that centre value is what `ground_at_tile()` /
 *      `TerrainField::set_water()`'s own dryness test reads (a bilinear blend of the nearest
 *      samples around it; one raw sample when `height_sub` is even and the centre lands exactly on
 *      one), not the block's raw mean, which only coincide at `height_sub == 1`. Lowering the
 *      block by a uniform amount moves the centre's bilinear blend by that same amount, so this is
 *      still the minimal fix for a wet tile whose corners sample a hair above its own surface, and
 *      it leaves whatever grading the ground already had (a mapcoopa lake bed, a sculpted shelf)
 *      otherwise alone.
 *   2. Holds every sample whose containing tiles are ALL wet -- the channel's interior, which no
 *      dry ground depends on -- `clearance` under the lowest level standing over it. Pass 1's
 *      centre-only guarantee is not enough where the bed is steep: a uniform block drop leaves a
 *      wet tile's uphill samples above that tile's own level, and the water surface, which sits
 *      exactly at the level, cuts through them. A sample shared with any dry tile is untouched,
 *      so the bank and the waterline keep their shape and this stays strictly gentler on them
 *      than pass 1's whole-block drop.
 *   3. Only with `grade.depth > 0`: an additional per-corner ceiling that reaches `grade.depth`
 *      deeper by `grade.band` tiles in from the shoreline (chamfer distance over the wet mask),
 *      for authors who want a guaranteed drop-off on an otherwise flat bed. Guarded to a no-op at
 *      the shoreline tile itself, so it only digs further in than pass 1 already did and never
 *      flattens the beach.
 *
 * @param heights `nx` x `ny` height samples, row-major, corner-addressed (one more than the tiles
 *                each way).
 * @param water   `water_w` x `water_h` cells, row-major, one per tile -- `water_w == nx - 1` and
 *                `water_h == ny - 1` is the ordinary case (a corner grid over a tile grid); passing
 *                anything else is a silent no-op help for callers over a differently-shaped window.
 */
void grade_water_shore(std::vector<float>& heights, int nx, int ny, const std::vector<WaterCell>& water,
                       int water_w, int water_h, const ShoreGrade& grade);

/**
 * @brief The same, over a built field's own height plane, for `rect` (tiles). Marks the height
 *        texels it moved dirty and returns them.
 *
 * Requires a height plane (`field.has_height_plane()`); a no-op on a field with none. Works at
 * any `height_sub`: each wet tile's corner block is `height_sub + 1` samples a side.
 */
IRect grade_water_shore(TerrainField& field, const IRect& rect, const ShoreGrade& grade);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_WATER_H
