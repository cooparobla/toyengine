/**
 * @file tile_edit.h
 * @brief Editing a TileTerrain's ground: raise / lower, set height, paint a surface, smooth --
 *        as pure functions over a TileMapData, for the editor's brushes AND for gameplay.
 *
 * These are the ground half of editing; tile_water.h is the water half, and every operation
 * here that moves ground ends by settling the water over it, because the two cannot be allowed
 * to disagree: a bed raised through its surface leaves land, and ground lowered beside water
 * floods. That is why each returns a rect that may reach well past the one it was given.
 *
 * `settle` is what lets a caller take the water part a frame at a time instead: with it off,
 * the ground moves and the water is left to a WaterSettle the caller drives (see
 * TerrainComponent::raise_tiles(), which is how gameplay edits). The editor leaves it on.
 *
 * No GPU, no Scene, no mapcoopa -- a walk over the tile grid, which is also what makes these
 * testable with nothing but a TileMapData.
 */

#ifndef TOYENGINE_WORLD_TILE_EDIT_H
#define TOYENGINE_WORLD_TILE_EDIT_H

#include <glm/glm.hpp>

#include <toyengine/world/planet.h>
#include <toyengine/world/tile_map_data.h>

namespace toy {
namespace world {

/**
 * @brief Raises (delta > 0) or lowers every tile's ground in `rect` by `delta` steps; never
 *        below 0. Under water the bed moves.
 *
 * @param settle Settle the water over the moved ground before returning (see the file doc).
 * @param planet The cube-sphere on a planet, so the settle's spread wraps across faces.
 * @return The tiles changed -- past `rect` when water ran in or off.
 */
IRect tile_raise(TileMapData& d, const IRect& rect, int delta, bool settle = true,
                 const PlanetLayout* planet = nullptr);

/** @brief Sets every tile's ground in `rect` to `steps` (under water, the bed), then settles. */
IRect tile_set_height(TileMapData& d, const IRect& rect, int steps, bool settle = true,
                      const PlanetLayout* planet = nullptr);

/** @brief Paints the top kind (and, with `side`, the matching side kind) -- beds under water included. */
IRect tile_paint(TileMapData& d, const IRect& rect, TileKind kind, bool side);

/**
 * @brief Moves each tile one step toward the median of its 3 x 3 neighbourhood (terraces stay
 *        whole steps), then settles the water.
 */
IRect tile_smooth(TileMapData& d, const IRect& rect, bool settle = true,
                  const PlanetLayout* planet = nullptr);

/**
 * @brief Water: `fill` floods the basin `rect` is in up to terrace `level` -- every tile
 *        connected to it whose ground is lower, as far as the ground holds the water;
 *        otherwise drains the whole body it touches. The returned rect is the flood's.
 */
IRect tile_water(TileMapData& d, const IRect& rect, int level, bool fill,
                 const PlanetLayout* planet = nullptr);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_EDIT_H
