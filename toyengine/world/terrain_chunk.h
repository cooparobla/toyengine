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
inline void mesh_chunk_columns_greedy(const ColumnPad& pad, const TileMeshLibrary& library,
                                      const TerrainParams& params, ChunkMeshData& out) {
    out.clear();
    if (!library.is_baked()) return;

    const glm::vec3    scale = params.cell_scale();
    const std::int32_t n     = params.chunk_size;
    auto origin_of = [&](std::int32_t lx, std::int32_t ly, std::int32_t cell) {
        return glm::vec3(static_cast<float>(lx) * params.tile_size, static_cast<float>(ly) * params.tile_size,
                         static_cast<float>(cell) * params.height_step);
    };

    // --- Tops ---
    if (library.side(TileFace::Top).mergeable) {
        std::vector<std::uint8_t> done(static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0);
        auto same = [&](std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by) {
            const TileColumn& a = pad.at(ax, ay);
            const TileColumn& b = pad.at(bx, by);
            return a.steps == b.steps && a.top_kind == b.top_kind;
        };
        for (std::int32_t ly = 0; ly < n; ++ly) {
            for (std::int32_t lx = 0; lx < n; ++lx) {
                if (done[static_cast<std::size_t>(ly * n + lx)]) continue;
                std::int32_t w = 1;
                while (lx + w < n && !done[static_cast<std::size_t>(ly * n + lx + w)] && same(lx, ly, lx + w, ly)) ++w;
                std::int32_t h = 1;
                for (; ly + h < n; ++h) {
                    bool row_ok = true;
                    for (std::int32_t x = lx; x < lx + w && row_ok; ++x) {
                        row_ok = !done[static_cast<std::size_t>((ly + h) * n + x)] && same(lx, ly, x, ly + h);
                    }
                    if (!row_ok) break;
                }
                for (std::int32_t y = ly; y < ly + h; ++y)
                    for (std::int32_t x = lx; x < lx + w; ++x) done[static_cast<std::size_t>(y * n + x)] = 1;
                const TileColumn& column = pad.at(lx, ly);
                library.append_span(TileFace::Top, origin_of(lx, ly, column.steps - 1), scale,
                                    glm::vec3(static_cast<float>(w), static_cast<float>(h), 1.0f),
                                    column.top_kind, out.vertices, out.indices);
            }
        }
    } else {
        for (std::int32_t ly = 0; ly < n; ++ly)
            for (std::int32_t lx = 0; lx < n; ++lx) {
                const TileColumn& column = pad.at(lx, ly);
                library.append(TileFace::Top, origin_of(lx, ly, column.steps - 1), scale, column.top_kind,
                               out.vertices, out.indices, true);
            }
    }

    if (params.emit_bottom) {
        for (std::int32_t ly = 0; ly < n; ++ly)
            for (std::int32_t lx = 0; lx < n; ++lx)
                library.append(TileFace::Bottom, origin_of(lx, ly, 0), scale, pad.at(lx, ly).side_kind,
                               out.vertices, out.indices, true);
    }

    // --- Walls ---
    struct Run {
        std::int32_t lo = 0, hi = 0;   // cell range, inclusive
        TileKind     kind = TileKind::Stone;
        bool         used = false;
    };
    std::vector<std::vector<Run>> runs(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (const TileFace face : k_lateral_faces) {
        const glm::ivec2 step = face_step(face);
        const bool mergeable  = library.side(face).mergeable;
        // Same exposure walk as mesh_chunk_columns(), collapsed into same-kind runs.
        for (std::int32_t ly = 0; ly < n; ++ly) {
            for (std::int32_t lx = 0; lx < n; ++lx) {
                std::vector<Run>& col_runs = runs[static_cast<std::size_t>(ly * n + lx)];
                col_runs.clear();
                const TileColumn& column    = pad.at(lx, ly);
                const TileColumn& neighbour = pad.at(lx + step.x, ly + step.y);
                if (neighbour.steps >= column.steps) continue;
                const std::int32_t top_cell    = column.steps - 1;
                const std::int32_t lowest_cell = std::max(neighbour.steps, column.steps - params.max_wall_steps);
                for (std::int32_t cell = top_cell; cell >= lowest_cell; --cell) {
                    const std::int32_t depth = top_cell - cell;
                    const TileKind kind = (depth < params.soil_depth_steps || column.water)
                                              ? column.side_kind : TileKind::Stone;
                    if (!mergeable) {
                        library.append(face, origin_of(lx, ly, cell), scale, kind, out.vertices, out.indices, true);
                        continue;
                    }
                    if (!col_runs.empty() && col_runs.back().kind == kind && col_runs.back().lo == cell + 1) {
                        col_runs.back().lo = cell;
                    } else {
                        col_runs.push_back(Run{cell, cell, kind, false});
                    }
                }
            }
        }
        if (!mergeable) continue;

        // Merge identical runs sideways, along the face's in-plane horizontal axis.
        const bool along_x = (face == TileFace::North || face == TileFace::South);
        auto find_run = [&](std::int32_t lx, std::int32_t ly, const Run& r) -> Run* {
            if (lx >= n || ly >= n) return nullptr;
            for (Run& c : runs[static_cast<std::size_t>(ly * n + lx)]) {
                if (!c.used && c.lo == r.lo && c.hi == r.hi && c.kind == r.kind) return &c;
            }
            return nullptr;
        };
        for (std::int32_t ly = 0; ly < n; ++ly) {
            for (std::int32_t lx = 0; lx < n; ++lx) {
                for (Run& r : runs[static_cast<std::size_t>(ly * n + lx)]) {
                    if (r.used) continue;
                    r.used = true;
                    std::int32_t m = 1;
                    while (Run* next = along_x ? find_run(lx + m, ly, r) : find_run(lx, ly + m, r)) {
                        next->used = true;
                        ++m;
                    }
                    const float height = static_cast<float>(r.hi - r.lo + 1);
                    const glm::vec3 span = along_x ? glm::vec3(static_cast<float>(m), 1.0f, height)
                                                   : glm::vec3(1.0f, static_cast<float>(m), height);
                    library.append_span(face, origin_of(lx, ly, r.lo), scale, span, r.kind,
                                        out.vertices, out.indices);
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
    if (params.greedy_merge) {
        mesh_chunk_columns_greedy(pad, library, params, out);
    } else {
        mesh_chunk_columns(pad, library, params, out);
    }
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
        // Off the main thread like the meshing itself: the mesher emits every side as its own
        // vertices, so shared corners weld back together here.
        coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
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
