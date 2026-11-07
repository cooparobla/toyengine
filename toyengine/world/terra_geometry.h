/**
 * @file terra_geometry.h
 * @brief The authored shape of a `TerraData`: a tile grid, its sub-resolutions, and its vertical
 *        scale.
 *
 * The tile grid is the ONE authority -- every plane's extent derives from it -- and there is no
 * mode-based invariant: `tiles_x` and
 * `tiles_y` are independent (a grid may be rectangular), and `height_sub` is not pinned to 1 by
 * anything here. Whether a terrain draws terraced or smooth is a MESHING STYLE choice made
 * elsewhere, over this same geometry; a `height_sub == 1` field simply has no height plane to
 * draw smooth with.
 *
 * Changing any of this reshapes the data (heights resampled, splats resampled, tiles kept where
 * they still exist -- see `TerraData::reshape()`), so the editor stages it behind an Apply.
 */

#ifndef TOYENGINE_WORLD_TERRA_GEOMETRY_H
#define TOYENGINE_WORLD_TERRA_GEOMETRY_H

#include <glm/glm.hpp>

namespace toy {
namespace world {

/**
 * @struct TerraGeometry
 * @brief See the file comment.
 */
struct TerraGeometry {
    int tiles_x = 0; /**< @brief Tiles along +x. Independent of tiles_y -- no forced-square rule. */
    int tiles_y = 0; /**< @brief Tiles along +y. */

    /** @brief Metres per tile edge, both axes. A tile is always square in metres. */
    float tile_size = 1.0f;

    /**
     * @brief Height samples per tile edge; the height plane is `tiles * height_sub + 1` per
     *        side. `1` means no height plane at all -- the tile's own `steps` IS its height,
     *        which is what a terraced meshing style reads.
     */
    int height_sub = 1;

    /** @brief Layer (splat) texels per tile edge; the layer planes are `tiles * layer_sub` per side. */
    int layer_sub = 1;

    /** @brief Metres per step: the vertical quantum a tile's integer `steps` counts in. */
    float height_step = 1.0f;

    /** @brief Metres at a normalised height of 1 -- the vertical scale of the float height plane. */
    float height = 64.0f;

    /** @brief Tiles per chunk edge -- the ONE chunk unit: streaming, drawing and collision alike. */
    int chunk_tiles = 16;

    /** @brief Metres covered, x and y: `{tiles_x, tiles_y} * tile_size`. */
    glm::vec2 size() const { return glm::vec2(static_cast<float>(tiles_x), static_cast<float>(tiles_y)) * tile_size; }

    /** @brief Metres per tile, x and y -- always equal, kept for callers that still ask per axis. */
    glm::vec2 tile_extent() const { return glm::vec2(tile_size); }

    /** @brief Height samples per side along x. */
    int height_nx() const { return tiles_x * height_sub + 1; }
    /** @brief Height samples per side along y. */
    int height_ny() const { return tiles_y * height_sub + 1; }
    /** @brief Layer texels along x. */
    int layer_nx() const { return tiles_x * layer_sub; }
    /** @brief Layer texels along y. */
    int layer_ny() const { return tiles_y * layer_sub; }

    /** @brief True when a height plane exists at all (height_sub > 1). */
    bool has_height_plane() const { return height_sub > 1; }

    /** @brief Metres between adjacent height samples -- uniform, since tiles are square. */
    float cell() const { return height_sub > 0 ? tile_size / static_cast<float>(height_sub) : tile_size; }

    /** @brief Chunks along x and y (ceil, at least one each). */
    glm::ivec2 chunk_count() const;

    /** @brief Clamps every field into its supported range. No cross-field invariant is forced. */
    TerraGeometry sanitized() const;

    bool operator==(const TerraGeometry& o) const {
        return tiles_x == o.tiles_x && tiles_y == o.tiles_y && tile_size == o.tile_size &&
               height_sub == o.height_sub && layer_sub == o.layer_sub && height_step == o.height_step &&
               height == o.height && chunk_tiles == o.chunk_tiles;
    }
    bool operator!=(const TerraGeometry& o) const { return !(*this == o); }
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRA_GEOMETRY_H
