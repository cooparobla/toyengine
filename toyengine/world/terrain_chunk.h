/**
 * @file terrain_chunk.h
 * @brief One chunk of terrain: sampling its columns, merging every exposed side into a single
 *        vertex/index buffer, and the bookkeeping the streamer wraps around that.
 *
 * The meshing half of this file is **pure**: it reads a sampler and a baked side library, and
 * writes a ChunkMeshData. No Vulkan, no Scene, no AssetManager, no globals. That is what lets
 * it run on a coopa::job worker (which is where it does run -- see terrain_system.h) and what
 * lets the `world` test group exercise it with no device at all.
 *
 * ### Face exposure, and the one-tile pad
 *
 * A column emits its top always, and a lateral face only for the steps that stand proud of the
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
#include <toyengine/world/tile_mesh_library.h>

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
 * @brief Samples every column a chunk needs, including its one-tile neighbour skirt.
 *
 * The only part of meshing that touches the map. Split out from mesh_chunk_columns() so the
 * merge can be tested against a hand-built pad with no generated world behind it.
 *
 * @param sampler The built world sampler; read concurrently by every chunk job.
 * @param params  The tiling.
 * @param coord   Which chunk to sample.
 * @param out     Receives `(chunk_size + 2)^2` columns.
 */
void sample_chunk_columns(const TerrainSampler& sampler, const TerrainParams& params,
                                 const ChunkCoord& coord, ColumnPad& out);

/**
 * @brief Merges every exposed side of a chunk's columns into one vertex/index buffer.
 *
 * Pure, deterministic and allocation-light: the emission order is a fixed walk (row-major over
 * the tiles, top face then each lateral in `k_lateral_faces` order, walls top-down), so two
 * runs over the same pad produce byte-identical buffers. The streamer's job parallelism relies
 * on that -- chunks are meshed on whichever worker is free, and nothing downstream may depend on
 * which one.
 *
 * @param pad     The chunk's columns plus its neighbour skirt, from sample_chunk_columns().
 * @param library The baked side geometry.
 * @param params  The tiling; supplies the cell scale, the wall clamp and the soil depth.
 * @param out     Receives the merged geometry, in chunk-local object space. Cleared first.
 */
void mesh_chunk_columns(const ColumnPad& pad, const TileMeshLibrary& library,
                               const TerrainParams& params, ChunkMeshData& out);

/**
 * @brief The greedy counterpart of mesh_chunk_columns(): the same surface, in far fewer quads.
 *
 * - Tops: rectangles of equal (surface height, top kind) merge into one quad, grown greedily
 *   along +X then +Y.
 * - Walls: each exposed column face's cells split into runs of one TileKind (the soil band,
 *   then stone below it), each run one tall quad; a run also merges sideways across
 *   neighbouring columns whose same face carries the identical run.
 *
 * Only sides the library reports as mergeable (flat unit quads) are stretched; any other side
 * keeps the per-tile path. Every UV is written in tile space (TileMeshLibrary::encode_uv), so
 * the chunk must be drawn with the `terrain` surface shader. Deterministic like the per-tile
 * mesher: a fixed walk order, so equal pads produce byte-identical buffers.
 */
void mesh_chunk_columns_greedy(const ColumnPad& pad, const TileMeshLibrary& library,
                                      const TerrainParams& params, ChunkMeshData& out);

/**
 * @brief The styled counterpart of mesh_chunk_columns(): the same columns, assembled from a
 *        style's authored pieces by neighbourhood (see tile_topology.h) instead of one shape
 *        per side.
 *
 * A column is shaped by ONE style, its top kind's, from lip to base: an ACNH cliff is one
 * smooth shape all the way down, and its soil and stone bands are texture, not geometry. Per
 * column, in a fixed walk (row-major; top, then lateral faces in `k_lateral_faces` order, each
 * as its right half then its left half, cells top-down), so equal pads produce byte-identical
 * buffers exactly like the voxel meshers:
 *
 * - **Top**: four classified quadrants where any cardinal is exposed or a quadrant is a convex
 *   corner of its kind against level neighbours; every other tile (the overwhelmingly common
 *   plateau interior) is merged into flat rectangles of equal height and kind after the walk,
 *   like the voxel greedy mesher's tops.
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
 * @brief Samples and meshes one chunk -- the whole unit of work a chunk job performs.
 *
 * @param sampler The built world sampler.
 * @param library The baked side geometry.
 * @param params  The tiling.
 * @param coord   Which chunk to build.
 * @param out     Receives the merged geometry, in chunk-local object space.
 */
void build_chunk_mesh(const TerrainSampler& sampler, const TileMeshLibrary& library,
                             const TerrainParams& params, const ChunkCoord& coord,
                             ChunkMeshData& out);

/**
 * @struct ChunkBuildJob
 * @brief Everything one chunk-meshing job needs, plus the buffer it writes into.
 *
 * Bundled into a single heap object so the job's closure is one `shared_ptr` -- small enough to
 * live inside coopa::job::TaskWrapper's 48-byte inline buffer, so submitting a chunk allocates
 * nothing beyond this struct. Capturing the fields individually would spill the closure to
 * std::function's heap path on every submission.
 *
 * The shared ownership is also what makes cancellation safe: the streamer may drop its own
 * reference the instant a chunk leaves view, and a job still running writes into a buffer that
 * is still alive because the job itself holds the other reference.
 *
 * `sampler` and `library` are borrowed, not owned -- both live on the TerrainComponent, which
 * outlives every job it submits (the streamer waits for in-flight jobs before letting go).
 */
struct ChunkBuildJob {
    const TerrainSampler*  sampler = nullptr;
    const TileMeshLibrary* library = nullptr;
    TerrainParams          params;
    ChunkCoord             coord;
    ChunkMeshData          mesh; /**< The output; valid once the job's handle reports complete. */

    /** @brief The job body. Touches nothing but this object and the two borrowed, const inputs. */
    void run();
};

/** @brief The world-space origin of a chunk's minimum corner -- its SceneObject's position. */
glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_CHUNK_H
