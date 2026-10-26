/**
 * @file terrain_chunk.h
 * @brief One chunk of terrain: sampling its columns, assembling them from the tile sets' pieces
 *        into a single vertex/index buffer (and, optionally, a collision mesh), and the
 *        bookkeeping the streamer wraps around that.
 *
 * The meshing functions here are **pure**: they read a sampler and a baked tile library, and
 * write a ChunkMeshData. No Scene, no AssetManager, no globals. That is what lets them run on a
 * coopa::job worker (which is where they do run -- see terrain_system.h) and what lets the
 * `world` test group exercise them with no device at all.
 *
 * ### Why the GPU buffers are created here too
 *
 * ChunkBuildJob::run() also builds the chunk's `Mesh` -- on the worker, not on the owner thread.
 * A chunk is 20k-50k vertices, so creating and filling its buffers is two `vmaCreateBuffer`
 * calls and a couple of megabytes of memcpy, and doing that in the frame the chunk lands is a
 * visible hitch. It is safe off-thread by construction, not by luck:
 *
 *   - `coopa::gfx::memory::Allocator` does NOT pass VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT,
 *     so VMA synchronises itself, and `vkCreateBuffer` / `vkBindBufferMemory` need no external
 *     synchronisation of their own.
 *   - `Buffer`'s constructor does nothing but `vmaCreateBuffer`, and `Buffer::upload()` is a
 *     memcpy into that allocation's persistent mapping.
 *   - The buffers are private to this job until the owner thread publishes them, so no other
 *     thread -- and no in-flight GPU work -- can be looking at them. A cancelled job's mesh is
 *     destroyed with the job, which is likewise safe: nothing ever drew it.
 *
 * Everything that is genuinely single-owner stays on the owner thread: the AssetManager, the
 * Scene, and the physics world (see terrain_system.h's thread split).
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


#include <toyengine/world/terrain_params.h>
#include <toyengine/world/tile_map_data.h>
#include <toyengine/world/tile_mesh_library.h>

namespace coopa::physx::geometry {
class TriangleMesh;
}

namespace coopa::gfx::core {
class Device;
}
namespace coopa::gfx::memory {
class Allocator;
}

namespace toy {
namespace world {

/// Sampling a chunk straight from a generated map is the map's side of the bridge
/// (toyengine/mapgen/terrain_sampler.h); only the overload below names it, by reference.
class TerrainSampler;

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
    /**
     * Water depth (m) under each corner, from the tiles: mean of (surface - bed) over the
     * flooded tiles sharing the corner -- the same tile set the corner's height averages, read
     * from the pad, so two chunks assign their shared edge identical depths. 0 at a skirt corner
     * no flooded tile touches (the shoreline). WaterBody bakes with these instead of raycasts.
     */
    std::vector<float>         depths;
    /**
     * Wave height multiplier (0..1) at each corner: 1 over still water, TerrainParams::
     * river_wave_scale over a river, the mean of the flooded tiles sharing the corner between the
     * two -- the same tiles its height and depth average, so two meshes meeting at a corner scale
     * their waves identically there and never pull it apart.
     */
    std::vector<float>         wave_scales;
    std::vector<std::uint32_t> indices;
    /// A planet chunk's water is given in its own frame (WaterBody::local_frame): `frame` maps it
    /// to the chunk's space -- local Z out from the planet at the chunk's centre. Identity, and
    /// unused, on a flat terrain.
    glm::mat4                  frame{1.0f};
    bool                       local_frame = false;

    bool empty() const { return indices.empty(); }
    void clear() {
        positions.clear();
        uvs.clear();
        depths.clear();
        wave_scales.clear();
        indices.clear();
    }
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
 * @brief Which of a welded chunk's vertices sit on its SEAM with the neighbouring chunks -- one
 *        flag per vertex, 1 on the seam.
 *
 * These are the vertices a coarser level may not move. Chunks pick their level from their own
 * size on screen, so neighbours routinely draw at different levels and only meet without a gap
 * if both still carry the vertices they shared at full density. The seam is the chunk's edge in
 * tile space; a planet's chunk, already bent onto the globe, is read back into face space first
 * (planet.h).
 */
std::vector<std::uint8_t> chunk_seam_locks(const TerrainParams& params, const ChunkCoord& coord,
                                           const ChunkMeshData& mesh);

/**
 * @brief Appends the chunk's coarser levels to `mesh.indices` and returns its LOD table
 *        ([0] = the full chunk); empty when the terrain has no levels configured.
 *
 * What `ChunkBuildJob::run()` does between welding the chunk and uploading it. Every level
 * shares LOD 0's vertices and holds its seam (chunk_seam_locks()), so the levels cost index
 * ranges only and no two chunks can crack against each other whichever levels they draw.
 */
std::vector<coopa::gfx::engine::data::MeshLod> build_chunk_lods(const TerrainParams& params,
                                                                const ChunkCoord& coord,
                                                                ChunkMeshData& mesh);

/**
 * @brief A fingerprint of the two water surfaces a chunk was built with.
 *
 * What a rebuild compares to decide whether it may keep the chunk's existing water children
 * rather than hand back fresh ones -- see TerrainChunk::water_hash. Covers every field
 * WaterBody::set_geometry() is given, plus the planet frame.
 *
 * @param still   The chunk's sea and lakes.
 * @param flowing The chunk's rivers.
 * @return 0 when the chunk has no water at all, and otherwise a hash of both surfaces.
 */
std::uint64_t chunk_water_hash(const ChunkWaterData& still, const ChunkWaterData& flowing);

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
    /**
     * @brief Build `collision_mesh` and stop: no LOD levels, no water, no GPU buffers.
     *
     * What a chunk that is already drawing needs when it comes inside `collision_radius` --
     * see TerrainChunk::wants_collision. Meaningless without `collision`.
     */
    bool                   collision_only = false;
    /**
     * @brief Where `gpu_mesh`'s buffers are created. Null leaves `gpu_mesh` null and the CPU
     *        arrays for the caller to upload (what the pure `world` unit tests do).
     */
    coopa::gfx::core::Device*      device    = nullptr;
    coopa::gfx::memory::Allocator* allocator = nullptr;
    ChunkMeshData          mesh; /**< The CPU geometry; valid once the job's handle reports complete. */
    /**
     * @brief The chunk mesh's LOD table ([0] = the full chunk), built by run() from
     *        TerrainParams::chunk_lods; empty when the terrain has no levels configured.
     *
     * The levels' indices live in `mesh.indices` behind LOD 0's, so this is a few ranges, not
     * geometry. The renderer picks one per frame from the chunk's size on screen, and the
     * shadow views follow that choice (toy::render::select_lod()).
     */
    std::vector<coopa::gfx::engine::data::MeshLod> lods;
    /** @brief The chunk's GPU mesh, built by run(); null for an empty chunk (or no `device`). */
    std::shared_ptr<coopa::gfx::engine::data::Mesh> gpu_mesh;
    /**
     * @brief chunk_water_hash() of the two surfaces below, taken by run().
     *
     * Here rather than at the publish, because it walks every water vertex and the frame a
     * chunk lands in is reserved for the scene touches alone (see terrain_system.h). What
     * reads it is TerrainChunk::water_hash.
     */
    std::uint64_t water_hash = 0;
    /** @brief The chunk's collider surface, when `collision`; null for an empty chunk. */
    std::shared_ptr<const coopa::physx::geometry::TriangleMesh> collision_mesh;
    ChunkWaterData         water_still;   /**< Output: the sea and lakes over this chunk. */
    ChunkWaterData         water_flowing; /**< Output: the rivers over this chunk. */

    /** @brief The job body: mesh, water, collider, GPU buffers. Touches nothing but this
     *         object, the borrowed const library and (through VMA) the device. */
    void run();

    /**
     * @brief Drops every output but keeps the buffers' CAPACITY, so a pooled job's next build
     *        re-uses the allocation instead of malloc'ing a couple of megabytes again.
     *
     * Only the owner thread, and only once the job has really finished (see
     * TerrainComponent::recycle_build_job()).
     */
    void reset_outputs();
};

/** @brief The world-space origin of a chunk's minimum corner -- its SceneObject's position. */
glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord);

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_CHUNK_H
