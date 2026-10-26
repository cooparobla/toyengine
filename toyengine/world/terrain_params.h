/**
 * @file terrain_params.h
 * @brief How a tile terrain's world is cut into tiles and chunks.
 *
 * Passed by value everywhere -- the mesher, the streamer and the map bake all need it, and it is
 * small, immutable and copyable on purpose so a meshing job can hold its own copy. Authored on
 * the `Terrain` component in `Tiles` mode (see terrain_component.h).
 */

#ifndef TOYENGINE_WORLD_TERRAIN_PARAMS_H
#define TOYENGINE_WORLD_TERRAIN_PARAMS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include <toyengine/world/planet.h>
#include <toyengine/world/tile_types.h>   // ChunkCoord, which chunk_at() answers with

namespace toy {
namespace world {

/**
 * @struct TerrainParams
 * @brief How the map's continuous world is cut into discrete tiles and chunks.
 *
 * Authored in scene YAML on the `Terrain` component (see terrain_component.h) and then passed
 * by value everywhere -- the sampler, the chunk mesher and the streamer all need it, and it is
 * small, immutable and copyable on purpose so a meshing job can hold its own copy.
 */
struct TerrainParams {
    /**
     * @brief Tiles per mapcoopa grid unit -- the resolution the map is sampled at.
     *
     * One grid unit is one Voronoi cell's nominal width, so this is "how many blocks wide a map
     * cell is". Raising it resolves coastlines and river channels more finely at a quadratic
     * cost in tiles; it does NOT add detail the map does not have, beyond the fractal
     * displacement `MapConfig::terrain_roughness` already carries.
     */
    std::int32_t tiles_per_grid_unit = 4;

    /**
     * @brief The world's size in tiles per side -- how many tiles the map is rasterised into,
     *        whatever its cell count. 0 derives it: `grid_size * tiles_per_grid_unit`.
     *
     * Set, it wins over `tiles_per_grid_unit`: the map is sampled at `world_tiles / grid_size`
     * tiles per cell (fractional is fine), so the world's size and the map's detail are chosen
     * separately. The world spans `world_tiles * tile_size` world units.
     */
    std::int32_t world_tiles = 0;

    /** @brief Tiles per side for a map of `grid_size` cells. */
    std::int32_t tiles_for_grid(std::int32_t grid_size) const {
        return world_tiles > 0 ? world_tiles : grid_size * tiles_per_grid_unit;
    }

    /** @brief Horizontal size of one tile, in world units. */
    float tile_size = 1.0f;

    /** @brief Vertical size of one height step, in world units; the quantisation of the terrain. */
    float height_step = 1.0f;

    /** @brief World height of the full `[0, 1]` elevation range -- the vertical scale of the world. */
    float height_scale = 16.0f;

    /** @brief Tiles per chunk edge; a chunk is `chunk_size * chunk_size` columns. */
    std::int32_t chunk_size = 16;

    /**
     * @brief Chunks of terrain kept loaded in every direction from the camera's chunk.
     *
     * The live region is a square of side `2 * view_radius + 1`. It sets how much geometry is
     * resident and meshed; what is DRAWN is narrower -- toy::render::ToyRenderPipeline
     * frustum-culls every chunk against the camera and each shadow view separately.
     */
    std::int32_t view_radius = 3;

    /**
     * @brief Longest wall a single column may emit, in steps.
     *
     * A cliff's exposed face is as many steps as the drop, and a sea cliff against the ocean
     * floor can be dozens. Past this many steps the wall stops: the result is a skirt whose
     * bottom is hidden inside the neighbouring column's own geometry at any viewing angle a
     * ground-level camera can reach, for a bounded cost per column.
     */
    std::int32_t max_wall_steps = 24;

    /** @brief Steps of `side_kind` under a top face before a wall switches to bare Stone. */
    std::int32_t soil_depth_steps = 3;

    /**
     * @brief How far the water surface sits under the top of the terrace it fills, in steps.
     *
     * Water fills a column to a whole terrace (TileRecord::water_level()), and its banks stand
     * at least that high; drawn exactly at the terrace top, the surface would lie flush with
     * every bank top and fight it. Half a step puts the shoreline halfway down the bank's wall,
     * where whatever shape the tile set gives that wall -- rounded, square -- is the shoreline.
     */
    float water_inset = 0.5f;

    /**
     * @brief The same for rivers, in steps. A river runs in a channel one tile wide, its banks
     *        the terrace it fills to; at the sea's half step it reads as sunk in a trench, so it
     *        sits higher, just under the bank tops.
     */
    float river_inset = 0.2f;

    /**
     * @brief How tall a river's waves stand next to the still water's, 0..1.
     *
     * A river's surface is its current, not the sea's swell -- but a river and the lake it runs
     * into SHARE the vertices along their seam (build_chunk_water()), and the vertex stage
     * displaces a vertex from its own body's wave params. A body-wide difference therefore pulls
     * every shared vertex two ways until the two surfaces part and the bed shows through the gap.
     * So the difference is carried per corner instead (ChunkWaterData::wave_scales), meaned over
     * the tiles sharing each corner exactly as its height and depth are: both meshes read the
     * same value at the seam, and the waves ease across it over a tile.
     */
    float river_wave_scale = 0.25f;

    // --- Planets (planet.h) ---

    /** @brief A planet: six faces of `face_tiles^2` tiles bent onto a sphere, instead of one flat grid. */
    bool planet = false;
    /** @brief Planet only: tiles along one face's edge (a multiple of `chunk_size`). */
    std::int32_t face_tiles = 128;
    /**
     * @brief Most tiles one merged top or wall run may span; 0 is unlimited. A planet meshes every
     *        tile on its own (1): a merged quad bent onto the sphere would cut a chord under the
     *        tiles beside it and open cracks along its edges.
     */
    std::int32_t max_merge = 0;

    // --- Level of detail (terrain_chunk.cpp builds these into every chunk mesh) ---

    /**
     * @struct ChunkLod
     * @brief One coarser level of a chunk's mesh: how much of it survives, and when it is drawn.
     */
    struct ChunkLod {
        float ratio       = 0.0f;  /**< Triangles kept, as a fraction of the full chunk. 0 ends the table. */
        float screen_size = 0.0f;  /**< Drawn while the chunk covers less of the screen's height than this. */
    };

    /** @brief Most levels a chunk mesh carries, past LOD 0. */
    static constexpr std::size_t k_max_chunk_lods = 4;

    /**
     * @brief The coarser levels every chunk mesh is built with, coarsest last; a zero `ratio`
     *        ends the table (all zero: no LOD, chunks always draw at full density).
     *
     * The defaults coarsen only what is genuinely small on screen: a chunk covering more than
     * 14% of the screen's height is untouched, so a camera on the ground sees exactly the mesh
     * it always did, while a globe held whole in frame -- where one tile is a couple of pixels
     * and the terrain runs to four triangles a pixel -- draws a twentieth of them. A streamed
     * flat terrain only reaches these sizes with a large `view_radius`, which is the case that
     * wants them.
     *
     * Chunk boundaries are locked through every level (Mesh::simplify_levels()), so neighbours
     * at different levels meet on the same vertices and never crack.
     */
    std::array<ChunkLod, k_max_chunk_lods> chunk_lods{{{0.40f, 0.14f}, {0.15f, 0.07f}, {0.07f, 0.03f}, {}}};

    /** @brief The planet's cube-sphere (planet.h). */
    PlanetLayout layout() const { return PlanetLayout{face_tiles, tile_size}; }

    /** @brief World-space size of one chunk edge. */
    float chunk_world_size() const {
        return static_cast<float>(chunk_size) * tile_size;
    }

    /** @brief The per-axis scale of one tile cell, for TileMeshLibrary::append_styled(). */
    glm::vec3 cell_scale() const {
        return glm::vec3(tile_size, tile_size, height_step);
    }

    /** @brief The chunk containing a world position (ignoring z). */
    ChunkCoord chunk_at(const glm::vec3& world_position) const {
        const float size = chunk_world_size();
        return ChunkCoord{static_cast<std::int32_t>(std::floor(world_position.x / size)),
                          static_cast<std::int32_t>(std::floor(world_position.y / size))};
    }
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_PARAMS_H
