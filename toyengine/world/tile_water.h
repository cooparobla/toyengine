/**
 * @file tile_water.h
 * @brief Editing the water over a TileTerrain's tiles: flooding a basin, draining a body, and
 *        keeping water consistent with the ground after an edit moves it.
 *
 * The bake places water where mapcoopa says it is (terrain_sampler.h); these are for what comes
 * after -- the editor's Tile Mode and gameplay. They keep the one invariant the bake establishes:
 * **water never stands above dry ground beside it.** Water is a depth over the ground
 * (TileRecord::water_depth), filling a tile to a whole terrace, so every operation here is a
 * walk over the tile grid -- no mapcoopa needed.
 *
 * Each returns the rect of tiles it changed (possibly past the one it was given -- a flood runs
 * as far as its basin does), and marks it dirty, so the caller can capture undo and the chunks
 * re-mesh.
 */

#ifndef TOYENGINE_WORLD_TILE_WATER_H
#define TOYENGINE_WORLD_TILE_WATER_H

#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/world/planet.h>
#include <toyengine/world/tile_map_data.h>

namespace toy {
namespace world {

/** @brief Default bound on how many tiles one flood or drain may change. */
inline constexpr std::size_t k_water_flood_cap = std::size_t{1} << 18;

/**
 * @brief Fills the basin under `level` from `seeds`: every tile 4-connected to a seed whose
 *        ground is below `level` and whose water (if any) is lower takes water to `level`.
 *        Still water (a lake; the sea when `ocean`). Stops after `cap` tiles.
 */
IRect flood_water(TileMapData& tiles, const std::vector<glm::ivec2>& seeds, int level, bool ocean = false,
                  std::size_t cap = k_water_flood_cap);

/** @brief Removes the water 4-connected to `seeds` (the whole body, at any level). Stops after `cap` tiles. */
IRect drain_water(TileMapData& tiles, const std::vector<glm::ivec2>& seeds, std::size_t cap = k_water_flood_cap);

/**
 * @brief Re-settles water after the ground in `rect` moved: water whose ground rose to its
 *        level is gone, and water beside ground that dropped below it spreads in (as far as the
 *        new basin runs, within `limit` and `cap`). The editor's ground brushes call this.
 *
 * @param limit Spreading never leaves this rect (clip it to keep an undo patch bounded); an
 *              empty rect means the whole grid.
 */
IRect settle_water(TileMapData& tiles, const IRect& rect, const IRect& limit = IRect{},
                   std::size_t cap = k_water_flood_cap);

/**
 * @brief The bake's rounding rule over `rect` (see TerrainSampler::bake()): a dry tile beside
 *        water rounds up to that water's terrace (the bank), a flooded tile's bed down to at
 *        least one step under it. Does not mark dirty -- the bake assigns the whole grid.
 */
void settle_water_rounding(TileMapData& tiles, const IRect& rect, const PlanetLayout* planet = nullptr);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_WATER_H
