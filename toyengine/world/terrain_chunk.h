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

#include <gfxcoopa/engine/data/mesh.h>

#include <toyengine/world/terrain_sampler.h>
#include <toyengine/world/tile_mesh_library.h>
#include <toyengine/world/tile_types.h>

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

    void clear() {
        vertices.clear();
        indices.clear();
    }
};

/**
 * @struct ColumnPad
 * @brief A chunk's columns plus a one-tile skirt of its neighbours', in local chunk coordinates.
 *
 * Indexed by local tile coordinates running `[-1, chunk_size]` on both axes: `at(-1, 0)` is the
 * neighbouring chunk's edge column, `at(0, 0)` this chunk's corner.
 */
struct ColumnPad {
    std::int32_t            chunk_size = 0;
    std::vector<TileColumn> columns;

    /** @brief Allocates the pad for a chunk of `size` tiles per edge. */
    void resize(std::int32_t size) {
        chunk_size = size;
        const std::size_t stride = static_cast<std::size_t>(size) + 2;
        columns.assign(stride * stride, TileColumn{});
    }

    /** @brief Mutable access by local tile coordinate, valid over `[-1, chunk_size]`. */
    TileColumn& at(std::int32_t local_x, std::int32_t local_y) {
        return columns[index_(local_x, local_y)];
    }

    /** @brief Read-only access by local tile coordinate, valid over `[-1, chunk_size]`. */
    const TileColumn& at(std::int32_t local_x, std::int32_t local_y) const {
        return columns[index_(local_x, local_y)];
    }

private:
    std::size_t index_(std::int32_t local_x, std::int32_t local_y) const {
        const std::int32_t stride = chunk_size + 2;
        const std::int32_t cx = std::clamp(local_x + 1, 0, stride - 1);
        const std::int32_t cy = std::clamp(local_y + 1, 0, stride - 1);
        return static_cast<std::size_t>(cy) * static_cast<std::size_t>(stride) +
               static_cast<std::size_t>(cx);
    }
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
inline void sample_chunk_columns(const TerrainSampler& sampler, const TerrainParams& params,
                                 const ChunkCoord& coord, ColumnPad& out) {
    out.resize(params.chunk_size);
    const std::int32_t base_x = coord.x * params.chunk_size;
    const std::int32_t base_y = coord.y * params.chunk_size;

    for (std::int32_t ly = -1; ly <= params.chunk_size; ++ly) {
        for (std::int32_t lx = -1; lx <= params.chunk_size; ++lx) {
            out.at(lx, ly) = sampler.sample(base_x + lx, base_y + ly);
        }
    }
}

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
inline void mesh_chunk_columns(const ColumnPad& pad, const TileMeshLibrary& library,
                               const TerrainParams& params, ChunkMeshData& out) {
    out.clear();
    if (!library.is_baked()) return;

    const glm::vec3 scale = params.cell_scale();

    for (std::int32_t ly = 0; ly < params.chunk_size; ++ly) {
        for (std::int32_t lx = 0; lx < params.chunk_size; ++lx) {
            const TileColumn& column = pad.at(lx, ly);

            const float world_x = static_cast<float>(lx) * params.tile_size;
            const float world_y = static_cast<float>(ly) * params.tile_size;

            // The topmost cell of the column spans steps [steps-1, steps], so its own +Z face --
            // the one the canonical mesh carries at local z = 1 -- lands exactly at the column's
            // surface height.
            const std::int32_t top_cell = column.steps - 1;
            library.append(TileFace::Top,
                           glm::vec3(world_x, world_y, static_cast<float>(top_cell) * params.height_step),
                           scale, column.top_kind, out.vertices, out.indices);

            if (params.emit_bottom) {
                library.append(TileFace::Bottom, glm::vec3(world_x, world_y, 0.0f), scale,
                               column.side_kind, out.vertices, out.indices);
            }

            for (const TileFace face : k_lateral_faces) {
                const glm::ivec2 step = face_step(face);
                const TileColumn& neighbour = pad.at(lx + step.x, ly + step.y);
                if (neighbour.steps >= column.steps) continue; // nothing of this side is exposed

                // Walk the wall downward from the top cell to the neighbour's surface, stopping
                // at the clamp -- see TerrainParams::max_wall_steps for why a bottomless cliff
                // is not worth paying for.
                const std::int32_t lowest_cell =
                    std::max(neighbour.steps, column.steps - params.max_wall_steps);
                for (std::int32_t cell = top_cell; cell >= lowest_cell; --cell) {
                    const std::int32_t depth = top_cell - cell;
                    const TileKind kind = (depth < params.soil_depth_steps || column.water)
                                              ? column.side_kind
                                              : TileKind::Stone;
                    library.append(face,
                                   glm::vec3(world_x, world_y,
                                             static_cast<float>(cell) * params.height_step),
                                   scale, kind, out.vertices, out.indices);
                }
            }
        }
    }
}

/**
 * @brief Samples and meshes one chunk -- the whole unit of work a chunk job performs.
 *
 * @param sampler The built world sampler.
 * @param library The baked side geometry.
 * @param params  The tiling.
 * @param coord   Which chunk to build.
 * @param out     Receives the merged geometry, in chunk-local object space.
 */
inline void build_chunk_mesh(const TerrainSampler& sampler, const TileMeshLibrary& library,
                             const TerrainParams& params, const ChunkCoord& coord,
                             ChunkMeshData& out) {
    ColumnPad pad;
    sample_chunk_columns(sampler, params, coord, pad);
    mesh_chunk_columns(pad, library, params, out);
}

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
    void run() {
        if (sampler == nullptr || library == nullptr) return;
        build_chunk_mesh(*sampler, *library, params, coord, mesh);
    }
};

/** @brief The world-space origin of a chunk's minimum corner -- its SceneObject's position. */
inline glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord) {
    const float size = params.chunk_world_size();
    return glm::vec3(static_cast<float>(coord.x) * size, static_cast<float>(coord.y) * size, 0.0f);
}

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_CHUNK_H
