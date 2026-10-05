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
#include <toyengine/world/tile_topology.h>
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
 * - **Top**: four classified quadrants where any cardinal is exposed; tiles with none (the
 *   overwhelmingly common plateau interior) are merged into flat rectangles of equal height and
 *   kind after the walk, like the voxel greedy mesher's tops.
 * - **Walls**: per exposed cell and half, the cap (top cell, with the lip) or plain piece for
 *   its end state. Straight halves are merged along each wall line into runs and stamped as
 *   one stretched strip per run -- most of a cliff is straight, and most of a lip's cost is
 *   its round-over rings.
 * - **Section caps** wherever the next piece across a boundary differs: the cell below where a
 *   wall's end state changes with depth, or the next wall along (a straight or concave end)
 *   where the neighbouring column has another style or ends its wall at a different height.
 * - **Feet** under convex halves at a real floor, covering the corner the rounding carved out.
 */
inline void mesh_chunk_styled(const ColumnPad& pad, const TileMeshLibrary& library,
                              const TerrainParams& params, ChunkMeshData& out) {
    out.clear();
    if (!library.has_styles()) return;

    using Plane = TileMeshLibrary::SectionPlane;
    const glm::vec3 scale = params.cell_scale();

    auto origin_of = [&](const glm::ivec2& c, std::int32_t cell) {
        return glm::vec3(static_cast<float>(c.x) * params.tile_size, static_cast<float>(c.y) * params.tile_size,
                         static_cast<float>(cell) * params.height_step);
    };
    auto wall_kind = [&](const TileColumn& col, std::int32_t cell) {
        const std::int32_t depth = col.steps - 1 - cell;
        return (depth < params.soil_depth_steps || col.water) ? col.side_kind : TileKind::Stone;
    };
    // Whether column `c` has a wall cell on `face` at `cell`, and whether that cell is its cap.
    struct WallCell {
        bool exists = false;
        bool cap    = false;
    };
    auto wall_cell_at = [&](const glm::ivec2& c, TileFace face, std::int32_t cell) {
        WallCell w;
        const TileColumn& col = pad.at(c);
        const TileColumn& nb  = pad.at(c + face_step(face));
        if (cell >= col.steps || cell < nb.steps || cell < col.steps - params.max_wall_steps) return w;
        w.exists = true;
        w.cap    = cell == col.steps - 1;
        return w;
    };
    auto stamp = [&](const TileMeshLibrary::SideGeometry& g, const glm::vec3& origin, TileKind top_kind,
                     TileKind wall, bool anchored) {
        library.append_styled(g, origin, scale, top_kind, wall, anchored, out.vertices, out.indices);
    };

    // Straight halves are not stamped where they are met: they are collected, then merged along
    // each wall line into runs and stamped once per run (see the end of this function). A
    // straight lip is an extrusion, so a run of twenty halves costs what one half does.
    struct StraightHalf {
        std::uint8_t turns;  // the face, as lateral_quarter_turns()
        std::int32_t cell;
        std::int32_t line;   // the column coordinate across the face
        std::int32_t slot;   // half-tile index along the face's axis
        std::uint8_t style;
        bool         cap;
        TileKind     top_kind;
        TileKind     wall_kind;
    };
    std::vector<StraightHalf> straights;
    // Tiles with no exposed edge, merged into rectangles after the walk like the voxel greedy
    // mesher's tops: a flat plateau is then a handful of quads, not two triangles a tile.
    std::vector<std::uint8_t> full(static_cast<std::size_t>(params.chunk_size) * static_cast<std::size_t>(params.chunk_size), 0);

    // A styled chunk runs ~20 triangles a tile; one up-front reservation saves the regrowth.
    const std::size_t tiles = static_cast<std::size_t>(params.chunk_size) * static_cast<std::size_t>(params.chunk_size);
    out.vertices.reserve(tiles * 48);
    out.indices.reserve(tiles * 64);

    for (std::int32_t ly = 0; ly < params.chunk_size; ++ly) {
        for (std::int32_t lx = 0; lx < params.chunk_size; ++lx) {
            const glm::ivec2  c(lx, ly);
            const TileColumn& col       = pad.at(c);
            const std::int32_t top_cell = col.steps - 1;
            const std::uint8_t style    = library.style_of(col.top_kind);
            auto exposed = [&](const glm::ivec2& dir) { return pad.at(c + dir).steps < col.steps; };

            // --- Top ---
            if (!exposed({0, 1}) && !exposed({0, -1}) && !exposed({1, 0}) && !exposed({-1, 0})) {
                full[static_cast<std::size_t>(ly * params.chunk_size + lx)] = 1;
            } else {
                for (std::uint8_t q = 0; q < 4; ++q) {
                    const PiecePlacement place = classify_quadrant(q, exposed(rotate_ccw({0, 1}, q)),
                                                                   exposed(rotate_ccw({1, 0}, q)));
                    stamp(library.piece(style, place.piece, place.orientation), origin_of(c, top_cell),
                          col.top_kind, col.top_kind, false);
                }
            }

            // --- Walls ---
            for (const TileFace face : k_lateral_faces) {
                const glm::ivec2  step = face_step(face);
                const TileColumn& nb   = pad.at(c + step);
                if (nb.steps >= col.steps) continue;

                const std::int32_t lowest = std::max(nb.steps, col.steps - params.max_wall_steps);
                for (const bool right : {true, false}) {
                    const glm::ivec2   along  = wall_end_direction(face, right);
                    const std::uint8_t orient = wall_orientation(face, right);

                    bool      have_above  = false;
                    WallEnd   above_end   = WallEnd::Continue;
                    TilePiece above_piece = TilePiece::WallContinue;
                    TileKind  above_kind  = TileKind::Stone;
                    bool      above_cap   = false;
                    WallEnd   end         = WallEnd::Continue;

                    for (std::int32_t cell = top_cell; cell >= lowest; --cell) {
                        end = classify_wall_end(pad, c, face, along, cell);
                        const bool      cap   = cell == top_cell;
                        const TileKind  wk    = wall_kind(col, cell);
                        const TilePiece piece = wall_piece(cap, end);
                        const glm::vec3 o     = origin_of(c, cell);

                        if (end == WallEnd::Continue) {
                            const bool along_x = face == TileFace::North || face == TileFace::South;
                            const std::int32_t t = along_x ? c.x : c.y;
                            const bool upper = (along_x ? along.x : along.y) > 0;
                            straights.push_back({lateral_quarter_turns(face), cell, along_x ? c.y : c.x,
                                                 2 * t + (upper ? 1 : 0), style, cap, col.top_kind, wk});
                        } else {
                            stamp(library.piece(style, piece, orient), o, col.top_kind, wk, cap);
                        }

                        if (have_above && above_end != end) {
                            stamp(library.section(style, piece, Plane::Top, orient), o, wk, wk, false);
                            stamp(library.section(style, above_piece, Plane::Bottom, orient),
                                  origin_of(c, cell + 1), above_kind, above_kind, above_cap);
                        }

                        if (end != WallEnd::Convex) {
                            const glm::ivec2 owner = end == WallEnd::Continue ? c + along : c + along + step;
                            const TileFace   owner_face = end == WallEnd::Continue ? face : face_of_step(-along);
                            const WallCell   next = wall_cell_at(owner, owner_face, cell);
                            if (next.exists && (library.style_of(pad.at(owner).top_kind) != style || next.cap != cap)) {
                                stamp(library.section(style, piece, Plane::End, orient), o, col.top_kind, wk, cap);
                            }
                        }

                        have_above  = true;
                        above_end   = end;
                        above_piece = piece;
                        above_kind  = wk;
                        above_cap   = cap;
                    }

                    if (lowest == nb.steps && end == WallEnd::Convex) {
                        stamp(library.foot(orient), origin_of(c, lowest), nb.top_kind, nb.top_kind, false);
                    }
                }
            }
        }
    }

    // --- Interior tops, as greedy rectangles of equal height and kind ---
    {
        const std::int32_t n = params.chunk_size;
        auto at = [&](std::int32_t x, std::int32_t y) -> std::uint8_t& {
            return full[static_cast<std::size_t>(y * n + x)];
        };
        auto same = [&](std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by) {
            const TileColumn& a = pad.at(ax, ay);
            const TileColumn& b = pad.at(bx, by);
            return at(bx, by) == 1 && a.steps == b.steps && a.top_kind == b.top_kind;
        };
        for (std::int32_t ly = 0; ly < n; ++ly) {
            for (std::int32_t lx = 0; lx < n; ++lx) {
                if (at(lx, ly) != 1) continue;
                std::int32_t w = 1;
                while (lx + w < n && same(lx, ly, lx + w, ly)) ++w;
                std::int32_t h = 1;
                for (; ly + h < n; ++h) {
                    bool row = true;
                    for (std::int32_t x = lx; x < lx + w && row; ++x) row = same(lx, ly, x, ly + h);
                    if (!row) break;
                }
                for (std::int32_t y = ly; y < ly + h; ++y)
                    for (std::int32_t x = lx; x < lx + w; ++x) at(x, y) = 2;
                const TileColumn& col = pad.at(lx, ly);
                library.append_styled(library.full_top(), origin_of({lx, ly}, col.steps - 1),
                                      glm::vec3(scale.x * static_cast<float>(w), scale.y * static_cast<float>(h), scale.z),
                                      col.top_kind, col.top_kind, false, out.vertices, out.indices);
            }
        }
    }

    // --- Straight runs ---
    // Sorted into wall lines, then each maximal run of adjacent halves sharing style, tier and
    // kinds becomes one stretched strip. The sort key is total, so the output stays deterministic.
    std::sort(straights.begin(), straights.end(), [](const StraightHalf& a, const StraightHalf& b) {
        if (a.turns != b.turns) return a.turns < b.turns;
        if (a.cell != b.cell) return a.cell < b.cell;
        if (a.line != b.line) return a.line < b.line;
        return a.slot < b.slot;
    });
    for (std::size_t i = 0; i < straights.size();) {
        const StraightHalf& first = straights[i];
        std::size_t j = i + 1;
        while (j < straights.size()) {
            const StraightHalf& h = straights[j];
            if (h.turns != first.turns || h.cell != first.cell || h.line != first.line ||
                h.slot != first.slot + static_cast<std::int32_t>(j - i) || h.style != first.style ||
                h.cap != first.cap || h.top_kind != first.top_kind || h.wall_kind != first.wall_kind) {
                break;
            }
            ++j;
        }
        const bool  along_x = first.turns == 0 || first.turns == 2; // North / South
        const float start   = 0.5f * static_cast<float>(first.slot) * params.tile_size;
        const float across  = static_cast<float>(first.line) * params.tile_size;
        const glm::vec3 origin(along_x ? start : across, along_x ? across : start,
                               static_cast<float>(first.cell) * params.height_step);
        library.append_styled(library.straight(first.style, first.cap, first.turns), origin, scale,
                              first.top_kind, first.wall_kind, first.cap, out.vertices, out.indices,
                              along_x ? 0 : 1, 0.5f * static_cast<float>(j - i));
        i = j;
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
    if (library.has_styles()) {
        mesh_chunk_styled(pad, library, params, out);
    } else if (params.greedy_merge) {
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
