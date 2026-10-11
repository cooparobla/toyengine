/**
 * @file terrain_chunk.h
 * @brief One chunk of terrain: sampling its columns, assembling them from the tile sets' pieces
 *        into a single vertex/index buffer (and, optionally, a collision mesh), and the
 *        bookkeeping the streamer wraps around that.
 *
 * The meshing half of this file is **pure**: it reads a sampler and a baked tile library, and
 * writes a ChunkMeshData. No Vulkan, no Scene, no AssetManager, no globals. That is what lets
 * it run on a coopa::job worker (which is where it does run -- see terrain_system.h) and what
 * lets the `world` test group exercise it with no device at all.
 *
 * ### Exposure, and the one-tile pad
 *
 * A column shows its top always, and a wall only for the steps that stand proud of the
 * neighbour on that side. Testing that at a chunk's edge needs the neighbouring chunk's
 * columns, so sampling covers `(chunk_size + 2)^2` columns -- the chunk plus a one-tile skirt --
 * and meshing only walks the interior. Without the pad, every chunk would assume air beyond its
 * border and wall itself in, and the world would be a grid of visible boxes.
 *
 * The pad is what makes chunks independent: two neighbouring chunks meshed on two different
 * threads, in either order, agree on their shared border because both sampled it.
 */

#ifndef TOYENGINE_WORLD_TERRAIN_CHUNK_H
#define TOYENGINE_WORLD_TERRAIN_CHUNK_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>


#include <toyengine/world/terrain_sampler.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/tile_mesh_library.h>

namespace coopa::physx::geometry {
class TriangleMesh;
}

namespace toy {
namespace world {

/**
 * @struct ChunkMeshData
 * @brief A chunk's merged CPU geometry, ready for Mesh::from_arrays().
 *
 * Object-space: positions are relative to the chunk's own origin, not the world's, so the chunk
 * SceneObject's Transform carries the placement and the vertex data stays small and reusable.
 */
struct ChunkMeshData {
    std::vector<coopa::gfx::engine::data::Vertex> vertices;
    std::vector<std::uint32_t>                    indices;

    /** @brief True when there is nothing to draw -- an all-air chunk beyond the map, say. */
    bool empty() const { return vertices.empty() || indices.empty(); }

    void clear();
};

/**
 * @struct ChunkWaterData
 * @brief One kind of a chunk's water surface (still or flowing), ready for
 *        toy::water::WaterBody::set_geometry(): triangle triples, chunk-local.
 */
struct ChunkWaterData {
    std::vector<glm::vec3>     positions;
    std::vector<glm::vec2>     uvs;      /**< Flowing: `u` runs downstream (water_flow_bake.h). */
    std::vector<std::uint32_t> indices;

    bool empty() const { return indices.empty(); }
    void clear() { positions.clear(); uvs.clear(); indices.clear(); }
};

/**
 * @brief Builds a chunk's water surfaces from its columns: one quad per flooded tile, at
 *        `water_inset` (`river_inset` for a river) under the terrace its water fills to, plus a skirt quad under every dry
 *        tile beside water.
 *
 * Each quad corner takes the mean height of the flooded tiles sharing it, so a river stepping
 * down a terrace becomes a slope and two chunks agree on their shared edge (both read it from
 * the pad). The skirt reaches under the banks: the bank is solid to at least the water's
 * terrace, so the skirt is hidden inside it, and the visible shoreline is wherever the flat
 * surface meets the bank's wall -- the tile set's shape, rounded or square. Still water (sea,
 * lake) and flowing water (river) are separate meshes, sharing their seam's vertices.
 *
 * @param pad     The chunk's columns plus skirt.
 * @param params  The tiling and `water_inset`.
 * @param still   Receives the sea and lakes.
 * @param flowing Receives the rivers.
 */
void build_chunk_water(const ColumnPad& pad, const TerrainParams& params, ChunkWaterData& still,
                       ChunkWaterData& flowing);

/**
 * @brief Samples every column a chunk needs, including its one-tile neighbour skirt, from the
 *        generated map directly (tests; baking goes through TerrainSampler::bake()).
 *
 * @param sampler The built world sampler.
 * @param params  The tiling.
 * @param coord   Which chunk to sample.
 * @param out     Receives `(chunk_size + 2)^2` columns.
 */
void sample_chunk_columns(const TerrainSampler& sampler, const TerrainParams& params,
                                 const ChunkCoord& coord, ColumnPad& out);

/**
 * @brief The same, from a terrain's stored tiles -- what streaming uses. Owner thread: the job
 *        gets the copy, never the TileMapData. Past the grid's edge the edge tiles repeat.
 */
void sample_chunk_columns(const TileMapData& tiles, const TerrainParams& params, const ChunkCoord& coord,
                          ColumnPad& out);

/**
 * @brief Assembles a chunk's columns from its tile sets' authored pieces, chosen by
 *        neighbourhood (see tile_topology.h). The terrain's one mesher: a rounded set and a
 *        square (voxel) set differ only in the pieces they hand it.
 *
 * A column is shaped by ONE tile set (style), its top kind's, from lip to base: a cliff is one
 * shape all the way down, and its soil and stone bands are colour, not geometry. Per column,
 * in a fixed walk (row-major; top, then lateral faces in `k_lateral_faces` order, each as its
 * right half then its left half, cells top-down), so equal pads produce byte-identical buffers
 * whichever worker meshed them:
 *
 * - **Top**: four classified quadrants where any cardinal is exposed or a quadrant is a convex
 *   corner of its kind against level neighbours (only for a set that rounds kind corners);
 *   every other tile (the overwhelmingly common plateau interior) is merged into flat
 *   rectangles of equal height and kind after the walk.
 * - **Surface blend codes** on every vertex (encode_surface_blend()): which kinds a surface may
 *   show, so terrain_styled.frag can round the corners where two kinds meet and let turf
 *   follow the curve of a lip.
 * - **Walls**: per exposed cell and half, the cap (top cell, with the lip) or plain piece for
 *   its end state. Straight halves are merged along each wall line into runs and stamped as
 *   one stretched strip per run -- most of a cliff is straight, and most of a lip's cost is
 *   its round-over rings. A cap run ending against a taller neighbour's wall ends in the
 *   continue taper, its lip easing out so the two walls meet square.
 * - **Section caps** wherever the next piece across a boundary differs: the cell below where a
 *   wall's end state changes with depth, or the next wall along (a straight or concave end)
 *   where the neighbouring column has another style.
 * - **Feet** under convex halves at a real floor, covering the corner the rounding carved out.
 */
void mesh_chunk_styled(const ColumnPad& pad, const TileMeshLibrary& library,
                              const TerrainParams& params, ChunkMeshData& out);

/**
 * @brief A static collision mesh of a chunk's drawn surface (welded, with adjacency and a BVH),
 *        or null for an empty chunk. Pure; run on the chunk's worker.
 */
std::shared_ptr<const coopa::physx::geometry::TriangleMesh> build_chunk_collision(const ChunkMeshData& mesh);

/**
 * @struct ChunkBuildJob
 * @brief Everything one chunk-meshing job needs, plus the buffers it writes into.
 *
 * Bundled into a single heap object so the job's closure is one `shared_ptr` -- small enough to
 * live inside coopa::job::TaskWrapper's 48-byte inline buffer, so submitting a chunk allocates
 * nothing beyond this struct.
 *
 * The shared ownership is also what makes cancellation safe: the streamer may drop its own
 * reference the instant a chunk leaves view, and a job still running writes into a buffer that
 * is still alive because the job itself holds the other reference.
 *
 * The columns are COPIED in (`pad`, sampled on the owner thread at submit), so the tiles may be
 * edited while the job runs. `library` is borrowed: it lives on the TerrainComponent, which
 * outlives every job it submits (the streamer waits for in-flight jobs before letting go).
 */
struct ChunkBuildJob {
    ColumnPad              pad;      /**< The chunk's columns plus its one-tile skirt. */
    const TileMeshLibrary* library = nullptr;
    TerrainParams          params;
    ChunkCoord             coord;
    bool                   collision = false; /**< Also build `collision_mesh`. */
    ChunkMeshData          mesh; /**< The output; valid once the job's handle reports complete. */
    /** @brief The chunk's collider surface, when `collision`; null for an empty chunk. */
    std::shared_ptr<const coopa::physx::geometry::TriangleMesh> collision_mesh;
    ChunkWaterData         water_still;   /**< Output: the sea and lakes over this chunk. */
    ChunkWaterData         water_flowing; /**< Output: the rivers over this chunk. */

    /** @brief The job body. Touches nothing but this object and the borrowed, const library. */
    void run();
};

/** @brief The world-space origin of a chunk's minimum corner -- its SceneObject's position. */
glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_CHUNK_H
