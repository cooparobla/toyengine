/**
 * @file terrain_water_mesh.h
 * @brief The water surface over a chunk of a SMOOTH terrain.
 *
 * The tile path has its own builder (`build_chunk_water()` in terrain_chunk.h): one quad per
 * flooded tile, inset under the terrace it fills, with a skirt tucked under the banks. A smooth
 * terrain needs the same *result* -- still and flowing meshes a WaterBody can take -- from
 * different ground, because it has no terraces and no walls for a shoreline to meet.
 *
 * ### Where the shoreline comes from
 *
 * Nowhere in particular: the surface is emitted flat at the water's level across every wet tile
 * **and one tile further**, and the terrain itself hides whatever lies under it. So the visible
 * waterline is exactly where the ground crosses the level -- the terrain's own slope is the
 * beach, and a gentle shore gives a wide one for free. That is the whole reason this is a
 * different builder rather than a parameter on the other: the tile path has to *choose* which
 * way a fractional shoreline rounds, and this one never faces the question.
 *
 * ### Why two chunks agree on their shared edge
 *
 * Every corner's height and depth is the mean over the wet tiles that TOUCH that corner, read
 * from the field by global tile coordinate. Two chunks meeting at an edge therefore average the
 * same tiles and write the same value, so their meshes meet exactly and no seam opens -- the
 * same guarantee the tile path gets from its one-tile pad.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_WATER_MESH_H
#define TOYENGINE_WORLD_TERRAIN_WATER_MESH_H

#include <glm/glm.hpp>

#include <toyengine/world/terrain_chunk.h>
#include <toyengine/world/terrain_field.h>

namespace toy {
namespace world {

/**
 * @brief Builds a smooth terrain chunk's water surfaces, chunk-local, ready for
 *        toy::water::WaterBody::set_geometry().
 *
 * @param field        The terrain, in `TerrainMode::Heightmap`. A Tiles field builds nothing
 *                     here -- `build_chunk_water()` is its path.
 * @param chunk        Which chunk, in chunk coordinates.
 * @param chunk_tiles  Tiles per chunk edge.
 * @param wave_scale_river Wave height multiplier over a river (TerrainParams::river_wave_scale);
 *                     still water is always 1.
 * @param still        Receives the sea and lakes.
 * @param flowing      Receives the rivers.
 */
void build_smooth_chunk_water(const TerrainField& field, glm::ivec2 chunk, int chunk_tiles,
                              float wave_scale_river, ChunkWaterData& still, ChunkWaterData& flowing);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_WATER_MESH_H
