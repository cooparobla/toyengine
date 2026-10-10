/**
 * @file water_tiles.h
 * @brief Splits a baked water surface into render tiles, each with distance LODs -- pure CPU, no
 *        GPU dependency (the result is MeshCpuData, uploaded by WaterSystem), so it is
 *        unit-testable on its own.
 *
 * Why. One mesh per water body is culled all-or-nothing and drawn at full density however far
 * away it is. Tiles let the renderer's frustum culling drop what is off screen, and each tile
 * carries its own LOD chain for the renderer's screen-size LOD selection (render/visibility.h).
 *
 * Procedural grids (WaterBody without mesh geometry) are cut into square tiles of 2^n quads.
 * LOD k keeps every 2^k-th grid row and column (always including the tile's last), so its
 * vertices are an exact SUBSET of LOD 0's: they carry LOD 0's baked depth, flow and turbulence
 * with no further baking, and two neighbouring tiles at any LODs still share their corners.
 *
 * Where LOD k may start. Two conditions, both as a distance:
 *  - Pixels: the level's vertex spacing subtends under 1/k_pixel_lod_factor radians. Over water
 *    seen at a grazing angle, triangles foreshorten to a fraction of that on screen, and
 *    sub-pixel triangles are what make the heavy water fragment shader expensive.
 *  - Cracks: mid-edge vertices of a finer neighbour are T-junctions against a coarser tile,
 *    displaced by waves the coarse edge interpolates linearly. The gap is the interpolation
 *    error of each wave over the coarse spacing, scaled by its distance fade
 *    (water_waves.h's wave_distance_fade()). It must stay under half a pixel there, so no
 *    skirts are needed (skirts would show through transparent water).
 * `lod_bias` (> 1 coarsens sooner) scales both. The renderer selects LODs by projected size, so
 * each distance is converted with an assumed 60-degree vertical field of view at 1080p; another
 * camera only shifts the switch distances.
 *
 * Arbitrary meshes (rivers, lake outlines) are bucketed into tiles by triangle centroid, and
 * each tile's LODs are simplified with meshoptimizer: meshopt_simplify keeps a SUBSET of the
 * tile's vertices (so baked attributes stay exact) and LockBorder pins every tile edge, so
 * neighbouring tiles stay stitched at any LOD pair. The same start-distance rule applies, with
 * the level's spacing estimated from its triangle count.
 */

#ifndef TOYENGINE_WATER_WATER_TILES_H
#define TOYENGINE_WATER_WATER_TILES_H

#include "water_parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>


#include <gfxcoopa/engine/data/mesh.h>


namespace toy {
namespace water {

/** @brief One render tile: its grid coordinate and its mesh (with LODs), in the body's local space. */
struct WaterTile {
    glm::ivec2 coord{0};
    coopa::gfx::engine::data::MeshCpuData data;
};

/** @brief Inputs to the LOD switch distances; see the file doc. */
struct WaterTileLodParams {
    float wave_lambdas[4] = {0.0f, 0.0f, 0.0f, 0.0f}; ///< Derived wavelengths, mesh units (0 = calm).
    float wave_amps[4]    = {0.0f, 0.0f, 0.0f, 0.0f}; ///< Their amplitudes, mesh units.
    float lod_bias = 1.0f;          ///< WaterSettings::lod_bias.
    int   max_lods = 6;
};

/// A LOD's vertex spacing must subtend under 1/this radians (~9 pixels at 1080p and a 60-degree
/// field of view, before a grazing view foreshortens it much further).
inline constexpr float k_pixel_lod_factor = 120.0f;
/// One pixel, in radians, at 1080p and a 60-degree vertical field of view.
inline constexpr float k_pixel_angle = 1.0f / 1030.0f;
/// proj[1][1] of the assumed 60-degree vertical field of view (1 / tan(30 deg)).
inline constexpr float k_assumed_p11 = 1.7320508f;

/**
 * @brief Worst T-junction gap against a LOD of vertex spacing `spacing` seen from `distance`:
 *        each wave's linear-interpolation error over the spacing, A (1 - cos(pi s / lambda)),
 *        times its distance fade.
 */
float water_lod_crack(float spacing, float distance, const WaterTileLodParams& p);

/**
 * @brief The nearest distance at which a LOD of vertex spacing `spacing` may be drawn: the
 *        pixel criterion met, and its T-junction gap under half a pixel (see the file doc).
 */
float water_lod_distance(float spacing, const WaterTileLodParams& p);

namespace detail {

/** @brief Fills bounds (inflated by `inflate` for the wave displacement the vertex shader adds)
 *         and rewinds every triangle of `d` so its normal faces local +Z (TransparentPass culls
 *         back faces). */
void finish_tile_(coopa::gfx::engine::data::MeshCpuData& d, const glm::vec3& inflate);

/** @brief Grid lines from `a` to `b` every `step`, always ending on `b`. */
std::vector<int> grid_lines_(int a, int b, int step);

} // namespace detail

/**
 * @brief Tiles a procedural grid of `res_x` x `res_y` quads whose (res_x+1)*(res_y+1) vertices
 *        are row-major in `verts` (LOD 0, fully baked), `quads_per_tile` per tile side.
 * @param spacing Vertex spacing of LOD 0, in the same (local) units as `lod`'s wavelengths.
 * @param inflate Bounds inflation for the wave displacement.
 */
std::vector<WaterTile> build_grid_tiles(const std::vector<coopa::gfx::engine::data::Vertex>& verts,
                                               int res_x, int res_y, int quads_per_tile, float spacing,
                                               const WaterTileLodParams& lod, const glm::vec3& inflate,
                                               const ParallelFor* par = nullptr);

namespace detail {

/** @brief Appends simplified LODs (vertex subsets, borders locked) to a single-level tile. */
void simplify_tile_lods_(coopa::gfx::engine::data::MeshCpuData& d, const WaterTileLodParams& lod,
                                const glm::vec3& inflate);

} // namespace detail

/**
 * @brief Tiles an arbitrary triangle mesh (`verts`/`indices`, local space) into `tile_size`
 *        squares in local XY by triangle centroid, each with a simplified LOD chain (given
 *        `lod`; null = single level).
 */
std::vector<WaterTile> build_mesh_tiles(const std::vector<coopa::gfx::engine::data::Vertex>& verts,
                                               const std::vector<uint32_t>& indices, float tile_size,
                                               const glm::vec3& inflate, const WaterTileLodParams* lod = nullptr,
                                               const ParallelFor* par = nullptr);

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_TILES_H
