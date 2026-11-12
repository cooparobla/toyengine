/**
 * @file planet_grid.h
 * @brief The TileGrid for a planet: six cube faces side by side in one grid (planet.h).
 *
 * TRANSITIONAL HOME. This belongs to the planet project, not the engine, and moves there with
 * the rest of the planet's terrain. It lives here for now only so the engine's existing planet
 * tests keep proving that routing the grid questions through TileGrid changed nothing.
 */

#ifndef TOYENGINE_WORLD_PLANET_GRID_H
#define TOYENGINE_WORLD_PLANET_GRID_H

#include <toyengine/world/planet.h>
#include <toyengine/world/tile_grid.h>

namespace toy {
namespace world {

/**
 * @class PlanetGrid
 * @brief Tiles as six cube faces bent onto a sphere, stored as one `6N x N` grid.
 *
 * Holds the layout rather than deriving it per call: `neighbour()` is asked once per cell per
 * flood-fill round and needs the face width, which only the layout carries. Construct it once
 * `face_tiles` is final -- the parser trims it to a multiple of `chunk_size`, and the stored
 * `.terra` holds the world at that width, so it cannot change afterwards anyway.
 */
class PlanetGrid final : public TileGrid {
public:
    /** @param layout The planet's cube-sphere, from `TerrainParams::layout()`. */
    explicit PlanetGrid(PlanetLayout layout) : layout_(layout) {}

    /** @brief Steps across a face's edge onto the next face, turned (PlanetLayout::neighbour). */
    glm::ivec2 neighbour(std::int32_t x, std::int32_t y,
                         std::int32_t dx, std::int32_t dy) const override;

    /** @brief The chunk's own face, then `PlanetLayout::wrap()` for anything past its edge. */
    glm::ivec2 tile_from(std::int32_t base_x, std::int32_t base_y,
                         std::int32_t lx, std::int32_t ly) const override;

    /**
     * @brief Which face and tile a direction out of the planet's centre lands on.
     *
     * A planet's tiles are not laid out under x and y, so a local position is read as a
     * direction rather than divided into a grid.
     */
    bool tile_at(const TerrainParams& params, const glm::vec3& local,
                 glm::ivec2& tile) const override;

    /** @brief Clipped to the face the brush started on: the next face is a world away in storage. */
    IRect clip_brush(const TerrainParams& params, const IRect& rect,
                     const glm::ivec2& at) const override;

    /** @brief Which of the six cube faces that column is on. */
    int region_at(const TerrainParams& params, std::int32_t x_tile) const override;

    /** @brief The point on the globe at that face-space position: a flat quad would cut under it. */
    glm::vec3 grid_point(const TerrainParams& params, double x_tiles, double y_tiles,
                         double height, int region = -1) const override;

    /**
     * @brief Marches the ray in from the outer sphere, then bisects onto the surface.
     *
     * Columns are tested by their tops only, so a wall hit reads as the column in front of it.
     */
    bool raycast(const TerraData& data, const TerrainParams& params, const glm::vec3& origin,
                 const glm::vec3& dir, float max_t, TileHit& hit) const override;

    /** @brief Yes: there is no "ahead" on a globe held whole in frame. */
    bool collides_everywhere() const override { return true; }

    /** @brief The globe itself: centred on the planet's centre, out to its tallest ground. */
    void world_bounds(const TerrainParams& params, float relief, glm::vec3& centre,
                      float& radius) const override;

private:
    PlanetLayout layout_;
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_PLANET_GRID_H
