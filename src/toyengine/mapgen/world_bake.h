/**
 * @file world_bake.h
 * @brief The seam a world that is not one flat square of tiles bakes its map through.
 *
 * `TerrainSampler::bake()` fills a `TerraData` by walking the tile grid in tile space: one square
 * of `tiles_per_axis()` across, each tile's point on the map read straight off its index. A world
 * whose geometry leaves the world plane (world/chunk_shaper.h) has a different grid and a
 * different map -- several faces stored side by side, points taken on a surface rather than a
 * plane -- and the engine models none of that.
 *
 * So the walk is replaceable. A project installs a hook; `bake()` offers it every bake and falls
 * back to the flat walk whenever the hook declines or there is none. The hook needs no privileged
 * access: `TerrainSampler::cell_at()` answers "the tile record at this point of the map" for an
 * arbitrary point, which is the only thing the flat walk knows that an arbitrary walk does not.
 */

#ifndef TOYENGINE_MAPGEN_WORLD_BAKE_H
#define TOYENGINE_MAPGEN_WORLD_BAKE_H

#include <functional>

namespace toy {
namespace world {

class TerraData;
class TerrainSampler;

/**
 * @brief A replacement for the flat tile walk.
 *
 * Reshapes `out` to the world's own grid (`TerraData::reset()`), fills every tile from
 * `sampler.cell_at()`, and reconciles the water rounding over it (world/terrain_water.h) --
 * everything `TerrainSampler::bake()` does for a flat square.
 *
 * @param sampler The built sampler, for its graph, its params and `cell_at()`.
 * @param out     The data to reshape and fill.
 * @return True once it has baked `out`; false to let the flat walk run instead, which is what a
 *         hook returns for a terrain that is not the shape it handles.
 */
using WorldBakeHook = std::function<bool(const TerrainSampler& sampler, TerraData& out)>;

/** @brief Installs the hook, replacing any previous one. An empty hook removes it. */
void set_world_bake_hook(WorldBakeHook hook);

/** @brief The installed hook, empty when there is none. */
const WorldBakeHook& world_bake_hook();

}   // namespace world
}   // namespace toy

#endif   // TOYENGINE_MAPGEN_WORLD_BAKE_H
