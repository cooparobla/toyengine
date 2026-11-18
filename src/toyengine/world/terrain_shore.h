/**
 * @file terrain_shore.h
 * @brief Grades a Tiles terrain's shorelines into embankments, shelving beds and beaches.
 *
 * A terraced field quantises its ground to whole steps, and `settle_water_rounding()`
 * (terrain_water.h) then raises every dry tile beside water to that water's terrace so no bank
 * ever stands under the water next to it. Both are necessary and both leave the shoreline as a
 * vertical riser: a steep coast or a cut river rim reads as a wall with no beach, because nothing
 * else has ever graded a shoreline in Tiles mode (`grade_water_shore()` is the smooth field's, and
 * a smooth field does not have the problem -- its shoreline is simply where the ground crosses the
 * water's level, so the terrain's own slope is the beach).
 *
 * `grade_tile_shore()` is the terraced answer: slope cones that cut a bank back to a walkable
 * grade, fill the lip the rounding leaves, shelve a deep bed up toward its margin, and retexture
 * the result as a beach where the ground ends up low and soft beside water. `ShoreProfile`
 * (terrain_params.h) is the shape they grade with and explains how a cone decides what to touch.
 *
 * Like every operation in terrain_water.h this takes an optional `TileGrid`: null is the flat
 * grid, and a layout holding several regions side by side passes its own so a shore grades
 * continuously across a region's edge instead of stopping at it.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_SHORE_H
#define TOYENGINE_WORLD_TERRAIN_SHORE_H

#include <toyengine/world/terrain_field.h>
#include <toyengine/world/terrain_params.h>
#include <toyengine/world/terrain_types.h>
#include <toyengine/world/tile_grid.h>

namespace toy {
namespace world {

/**
 * @brief Grades every shoreline in `rect` into an embankment, a shelving bed and a beach.
 *
 * Call AFTER `settle_water_rounding()`: the fill cone grades the lip that pass leaves, and the
 * ring it pins is what makes the cut cone provably unable to lower a waterline tile. A no-op
 * unless `params.shore.enabled`.
 *
 * Idempotent: running it again over already-graded ground changes nothing, which is what lets a
 * bake and a later edit both call it without the shore creeping further each time.
 *
 * @param field    The field to grade; mutated in place. Tiles mode only -- a field with a height
 *                 plane is left alone, since `grade_water_shore()` is its counterpart.
 * @param rect     Tile-coordinate rectangle to grade.
 * @param params   The terrain's parameters; `params.shore` is the profile and the rest fixes the
 *                 dither's lattice on the terrain's real surface.
 * @param topology The tile grid to step neighbours through, or null for the flat grid.
 */
void grade_tile_shore(TerrainField& field, const IRect& rect, const TerrainParams& params,
                      const TileGrid* topology = nullptr);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_SHORE_H
