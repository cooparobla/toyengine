/**
 * @file tile_grid.h
 * @brief How a terrain's tiles are laid out: which tile is next to which, and where each one is.
 *
 * A flat terrain's tiles are one rectangular grid under world X and Y. Every question about them
 * has an arithmetic answer -- the neighbour of `(x, y)` is `(x + dx, y + dy)`, the tile under a
 * point is that point divided by the tile size, a brush is a rectangle. A terrain whose tiles
 * wrap onto some other surface answers all of it differently, and a `TileGrid` is where
 * those answers live.
 *
 * **A null topology is the flat grid.** Every call site reads `topology != nullptr` and falls
 * back to the arithmetic it always used, so a flat terrain costs one null compare and keeps a
 * perfectly-predicted branch -- including inside the water flood fill, which asks for a
 * neighbour once per cell per round. Only a terrain with a real topology pays for the indirect
 * call, and it pays it where the work is an edit rather than a frame.
 *
 * Asked on the **owner thread** and by the editor -- the brush, the picking ray, the outline
 * overlay, the water settle. That is why this is a separate seam from
 * `world/chunk_shaper.h`, which is asked on a build worker: each states one threading contract
 * even when a project implements both with one class.
 *
 * Every method takes the `TerrainParams` it needs rather than caching them, so one instance
 * serves a terrain whose parameters are still being parsed, and holds no state to go stale.
 */

#ifndef TOYENGINE_WORLD_TILE_GRID_H
#define TOYENGINE_WORLD_TILE_GRID_H

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

struct TerrainParams;
struct TileHit;
class TerrainField;
class TerraData;

/**
 * @class TileGrid
 * @brief The layout of a terrain's tiles. Null means one flat rectangular grid under X and Y.
 */
class TileGrid {
public:
    virtual ~TileGrid() = default;

    /**
     * @brief The stored tile `(dx, dy)` away from `(x, y)`.
     *
     * The hot one: the water flood fill, the settle and the smooth all step their neighbours
     * through this, once per cell per round. Flat is `{x + dx, y + dy}`.
     */
    virtual glm::ivec2 neighbour(std::int32_t x, std::int32_t y,
                                 std::int32_t dx, std::int32_t dy) const = 0;

    /**
     * @brief The stored tile at `(lx, ly)` measured from a chunk's base corner.
     *
     * How a chunk reads its own columns and the one-tile skirt around them, so `lx` and `ly`
     * range over `[-1, chunk_size]` and a chunk at an edge of the layout asks for tiles beyond
     * it. Takes the base corner rather than an absolute coordinate because which region a tile
     * belongs to has to be decided from the CHUNK, not from the tile: a coordinate one past an
     * edge is ambiguous, and resolving it the other way can pick a neighbouring region without
     * the turn that joining it requires.
     */
    virtual glm::ivec2 tile_from(std::int32_t base_x, std::int32_t base_y,
                                 std::int32_t lx, std::int32_t ly) const = 0;

    /**
     * @brief Which tile a terrain-local point lies over.
     *
     * @param params The terrain's parameters.
     * @param local  The point, in the terrain object's local space.
     * @param tile   Receives the tile.
     * @return False when the point is over no tile at all.
     */
    virtual bool tile_at(const TerrainParams& params, const glm::vec3& local,
                         glm::ivec2& tile) const = 0;

    /**
     * @brief Clips a brush's tile range so it cannot reach storage that is nowhere near it.
     *
     * A rectangle around a tile is the right set of tiles on a flat grid. Where the grid holds
     * several regions side by side, a rectangle straddling a boundary runs into columns that
     * are far away in the world, so the range is clipped to the region the brush started in.
     *
     * @param params The terrain's parameters.
     * @param rect   The brush's unclipped range.
     * @param at     The tile the brush is centred on.
     * @return The range to edit.
     */
    virtual IRect clip_brush(const TerrainParams& params, const IRect& rect,
                             const glm::ivec2& at) const = 0;

    /**
     * @brief Which region of the stored grid a tile column belongs to; 0 where there is one.
     *
     * Fix this once and pass it to `grid_point()` for every point of an outline or a ring, so
     * a shape straddling a boundary still draws as one continuous piece in the region it
     * started in rather than jumping between them part-way round.
     */
    virtual int region_at(const TerrainParams& params, std::int32_t x_tile) const = 0;

    /**
     * @brief Where a point on the tile grid sits in the terrain's local space.
     *
     * The query every outline, brush ring and selection overlay is built from: a flat quad is
     * right only while the surface is flat, and anything else has to put each point on the real
     * surface or the overlay cuts under it.
     *
     * @param params   The terrain's parameters.
     * @param x_tiles  Position along the grid's first axis, in tiles (fractional is fine).
     * @param y_tiles  Position along the grid's second axis, in tiles.
     * @param height   Height above the datum, world units.
     * @param region   Which region of the grid to read `x_tiles` within, for a layout that
     *        holds several; -1 derives it from `x_tiles`. Pass an explicit one when walking a
     *        line of points that must all stay in the region it started in.
     * @return The point, terrain-local.
     */
    virtual glm::vec3 grid_point(const TerrainParams& params, double x_tiles, double y_tiles,
                                 double height, int region = -1) const = 0;

    /**
     * @brief Marches the stored columns for the first one a ray enters.
     *
     * @param data   The terrain's tiles.
     * @param params The terrain's parameters.
     * @param origin Ray origin, terrain-local.
     * @param dir    Unit ray direction, terrain-local.
     * @param max_t  How far to march.
     * @param hit    Receives the hit.
     * @return Whether anything was hit.
     */
    virtual bool raycast(const TerraData& data, const TerrainParams& params,
                         const glm::vec3& origin, const glm::vec3& dir, float max_t,
                         TileHit& hit) const = 0;

    /**
     * @brief Whether every chunk needs a collider, whatever its distance.
     *
     * A `collision_radius` keeps colliders near the camera, which is right when the ground
     * ahead is the only ground that matters. Where the whole terrain is in view at once,
     * solid ground in front of you and none behind reads worse than none at all.
     */
    virtual bool collides_everywhere() const = 0;

    /**
     * @brief A bounding sphere for the whole terrain, terrain-local -- what framing it needs.
     *
     * @param params The terrain's parameters.
     * @param relief The terrain's measured peak height above its datum.
     * @param centre Receives the centre.
     * @param radius Receives the radius.
     */
    virtual void world_bounds(const TerrainParams& params, float relief, glm::vec3& centre,
                              float& radius) const = 0;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TILE_GRID_H
