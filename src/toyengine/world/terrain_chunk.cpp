#include <toyengine/world/terrain_chunk.h>

#include <toyengine/world/chunk_shaper.h>
#include <toyengine/world/tile_grid.h>

#include <toyengine/mapgen/terrain_sampler.h>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <toyengine/world/tile_grid.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

void ChunkMeshData::clear() {
    vertices.clear();
    indices.clear();
    max_steps = 0;
}

void sample_chunk_columns(const TerrainSampler& sampler, const TerrainParams& params,
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

void sample_chunk_columns(const TerrainField& field, const TerrainParams& params, const ChunkCoord& coord,
                          ColumnPad& out, const TileGrid* topology) {
    out.resize(params.chunk_size);
    const std::int32_t base_x = coord.x * params.chunk_size;
    const std::int32_t base_y = coord.y * params.chunk_size;
    const float hstep = params.height_step;
    if (topology != nullptr) {
        // The skirt past a region's edge is whatever the layout joins it to, turned as the
        // layout turns it (world/tile_grid.h).
        for (std::int32_t ly = -1; ly <= params.chunk_size; ++ly) {
            for (std::int32_t lx = -1; lx <= params.chunk_size; ++lx) {
                const glm::ivec2 t = topology->tile_from(base_x, base_y, lx, ly);
                out.at(lx, ly) = tile_column(*field.record_at(t), hstep);
            }
        }
        return;
    }
    for (std::int32_t ly = -1; ly <= params.chunk_size; ++ly) {
        for (std::int32_t lx = -1; lx <= params.chunk_size; ++lx) {
            out.at(lx, ly) = tile_column(*field.record_at(glm::ivec2(base_x + lx, base_y + ly)), hstep);
        }
    }
}

void mesh_chunk_styled(const ColumnPad& pad, const TileMeshLibrary& library,
                              const TerrainParams& params, ChunkMeshData& out,
                              int style_override, int native_border) {
    out.clear();
    if (!library.has_styles()) return;

    // With an override (a ChunkLod::style far tier), every tile meshes in that style except
    // `native_border` steps of the chunk's rim, which keep their own -- see mesh_chunk_styled()'s
    // doc. Ordinary builds (style_override < 0) always take each tile's own style, unchanged.
    auto style_at = [&](const glm::ivec2& tile, TileKind kind) -> std::uint8_t {
        if (style_override < 0) return library.style_of(kind);
        if (native_border > 0 &&
            (tile.x < native_border || tile.y < native_border ||
             tile.x >= params.chunk_size - native_border || tile.y >= params.chunk_size - native_border)) {
            return library.style_of(kind);
        }
        return static_cast<std::uint8_t>(style_override);
    };

    // TOY_TERRAIN_NO_MERGE: diagnostic. Emits every top as its four quadrant pieces and every
    // wall half as its own stamp, bypassing the greedy top-rectangle merge and the straight-run
    // merge below. Merged geometry subdivides differently from the per-tile geometry it borders,
    // so every merge boundary is a line of T-junctions -- edges one side splits with a vertex
    // the other side does not share -- and those cannot rasterize watertight: sub-pixel holes
    // open and close along the line as the camera moves. With this set, every piece lies on the
    // uniform half-tile grid and the junctions disappear, at the cost of the triangle count the
    // merges exist to save -- which is what makes it an A/B for whether a surface sparkle is
    // the mesh or the shading.
    const bool no_merge = std::getenv("TOY_TERRAIN_NO_MERGE") != nullptr;

    using Plane = TileMeshLibrary::SectionPlane;
    const glm::vec3 scale = params.cell_scale();

    auto origin_of = [&](const glm::ivec2& c, std::int32_t cell) {
        return glm::vec3(static_cast<float>(c.x) * params.tile_size, static_cast<float>(c.y) * params.tile_size,
                         static_cast<float>(cell) * params.height_step);
    };
    auto wall_kind = [&](const TileColumn& col, std::int32_t cell) {
        const std::int32_t depth = col.steps - 1 - cell;
        return depth < params.soil_depth_steps ? col.side_kind : TileKind::Stone;
    };
    // Whether column `c` has a wall cell on `face` at `cell`.
    auto wall_cell_at = [&](const glm::ivec2& c, TileFace face, std::int32_t cell) {
        const TileColumn& col = pad.at(c);
        const TileColumn& nb  = pad.at(c + face_step(face));
        return cell < col.steps && cell >= nb.steps;
    };
    auto stamp = [&](const TileMeshLibrary::SideGeometry& g, const glm::vec3& origin, float code,
                     bool anchored) {
        library.append_styled(g, origin, scale, code, anchored, out.vertices, out.indices);
    };

    // Straight halves are not stamped where they are met: they are collected, then merged along
    // each wall line into runs and stamped once per run (see the end of this function). A
    // straight lip is an extrusion, so a run of twenty halves costs what one half does. Each
    // entry already represents a vertically-merged run of `cell_count` cells (see the per-tile
    // walk below), collapsed before this horizontal pass ever sees it -- so a tall straight
    // cliff costs one stamp both ways, not one per cell times one per tile.
    struct StraightHalf {
        std::uint8_t turns;       // the face, as lateral_quarter_turns()
        std::int32_t cell_bottom; // lowest cell of the vertically-merged run
        std::int32_t cell_count;  // cells spanned, bottom to top_cell-ward inclusive
        std::int32_t line;        // the column coordinate across the face
        std::int32_t slot;        // half-tile index along the face's axis
        std::uint8_t style;
        bool         cap;         // the lip tier (WallCapContinue) vs a plain wall (WallContinue)
        float        code;        // the cell's blend code (encode_surface_blend())
    };
    std::vector<StraightHalf> straights;
    // Flat full quadrants (TileMeshLibrary::top_merges()), merged into rectangles after the walk
    // (greedy, row-major, in half-tile units): a flat plateau is then a handful of quads, not
    // four stamps a tile -- and a tile with one mismatched corner still merges its other three.
    const std::int32_t qn = 2 * params.chunk_size;  // half-tile grid edge
    std::vector<std::uint8_t> full(static_cast<std::size_t>(qn) * static_cast<std::size_t>(qn), 0);

    // A styled chunk runs ~20 triangles a tile on a flat plateau; one up-front reservation saves
    // the regrowth. A cliff now costs a flat `soil_depth_steps + 1` wall bands a tile (vertical
    // merging below), not one band a height step, so the per-edge allowance is a small constant
    // rather than a function of height -- still only an estimate, not the exact size
    // append_styled() itself must never be called with (that regression is documented at its own
    // reserve-less loop).
    const std::size_t tiles = static_cast<std::size_t>(params.chunk_size) * static_cast<std::size_t>(params.chunk_size);
    const std::size_t wall_bands = static_cast<std::size_t>(std::max(1, params.soil_depth_steps + 1));
    out.vertices.reserve(tiles * (48 + 8 * wall_bands));
    out.indices.reserve(tiles * (64 + 12 * wall_bands));

    for (std::int32_t ly = 0; ly < params.chunk_size; ++ly) {
        for (std::int32_t lx = 0; lx < params.chunk_size; ++lx) {
            const glm::ivec2  c(lx, ly);
            const TileColumn& col       = pad.at(c);
            const std::int32_t top_cell = col.steps - 1;
            out.max_steps = std::max(out.max_steps, col.steps);
            const std::uint8_t style    = style_at(c, col.top_kind);
            // A tile set may keep kind boundaries square (a blocky set): then no top is a blend
            // corner, and every quadrant is plain. A terrain whose geometry is bent off the
            // world plane says the same through `round_kind_corners` -- there is no tile grid
            // left in world space to round against.
            const bool round_corners    = library.rounds_kind_corners(style) && params.allow_kind_corner_rounding;
            auto exposed = [&](const glm::ivec2& dir) { return pad.at(c + dir).steps < col.steps; };
            // A neighbour's kind as this top may blend into it: only across a level edge -- across
            // a cliff the two surfaces never touch.
            auto level_kind = [&](const glm::ivec2& dir) {
                const TileColumn& n = pad.at(c + dir);
                return n.steps == col.steps ? n.top_kind : col.top_kind;
            };

            // --- Top ---
            // terrain_styled.frag only acts where a quadrant is a convex corner of its own kind
            // against level neighbours -- the corners it rounds. Anywhere else a quadrant's
            // piece is a flat full quadrant (library.top_merges()), so it joins the greedy
            // rectangle merge below instead of being stamped on its own -- quadrant by quadrant,
            // so a tile with one mismatched corner still merges its other three.
            for (std::uint8_t q = 0; q < 4; ++q) {
                const glm::ivec2 cy = rotate_ccw({0, 1}, q), cx = rotate_ccw({1, 0}, q);
                const PiecePlacement place = classify_quadrant(q, exposed(cy), exposed(cx));
                const glm::ivec2 sgn = cx + cy; // the quadrant's world directions, each +-1
                const TileKind kx = level_kind({sgn.x, 0}), ky = level_kind({0, sgn.y});
                const bool corner = round_corners && kx != col.top_kind && ky != col.top_kind;
                if (!no_merge && !corner && library.top_merges(style, place.piece)) {
                    const std::int32_t qx = 2 * lx + (sgn.x > 0 ? 1 : 0);
                    const std::int32_t qy = 2 * ly + (sgn.y > 0 ? 1 : 0);
                    full[static_cast<std::size_t>(qy) * static_cast<std::size_t>(qn) +
                        static_cast<std::size_t>(qx)] = 1;
                    continue;
                }
                const float code = corner
                    ? encode_surface_blend(SurfaceBlend::Top, col.top_kind, kx, ky, level_kind(sgn), sgn.x, sgn.y)
                    : encode_surface_flat(col.top_kind);
                stamp(library.piece(style, place.piece, place.orientation), origin_of(c, top_cell), code, false);
            }

            // --- Walls ---
            for (const TileFace face : k_lateral_faces) {
                const glm::ivec2  step = face_step(face);
                const TileColumn& nb   = pad.at(c + step);
                if (nb.steps >= col.steps) continue;

                // The wall runs the whole drop, down to the neighbour's own height: a wall
                // stopping short would leave an open band the width of the tile edge, which
                // back-face culling turns into a see-through hole. Depth is free here -- the
                // vertical run-merge below collapses a plain wall to `soil_depth_steps + 1`
                // stamps whatever its height -- so there is nothing to trade for it.
                const std::int32_t lowest = nb.steps;
                for (const bool right : {true, false}) {
                    const glm::ivec2   along  = wall_end_direction(face, right);
                    const std::uint8_t orient = wall_orientation(face, right);

                    bool      have_above  = false;
                    WallEnd   above_end   = WallEnd::Continue;
                    TilePiece above_piece = TilePiece::WallContinue;
                    float     above_code  = 0.0f;
                    bool      above_cap   = false;
                    WallEnd   end         = WallEnd::Continue;

                    // A pending vertical run: consecutive cells of this same (tile, face, half)
                    // sharing one piece and surface code, accumulated top-down and stamped once
                    // when it breaks. `wall_kind()` only changes at the soil/stone boundary, so a
                    // plain wall collapses to at most `soil_depth_steps + 1` runs whatever its
                    // height -- the fix for height scaling the whole chunk's cost. Valid on a
                    // planet too: these pieces are a vertical extrusion of the plan profile
                    // (terrain/README.md), and a radial edge stays straight under
                    // ChunkShaper::shape()'s per-vertex map, unlike a horizontal merge, which
                    // is why only this axis is open to `max_merge == 1`.
                    bool         pending_active = false;
                    TilePiece    pending_piece  = TilePiece::WallContinue;
                    float        pending_code   = 0.0f;
                    std::int32_t pending_bottom = 0;
                    std::int32_t pending_count  = 0;

                    auto flush_pending = [&]() {
                        if (!pending_active) return;
                        pending_active = false;
                        if (pending_piece == TilePiece::WallContinue || pending_piece == TilePiece::WallCapContinue) {
                            // Folds into the horizontal pass below exactly as an unmerged cell
                            // would have: a cap run is always one cell (no lower cell shares its
                            // piece), so this is a no-op widening for caps and the real collapse
                            // for plain wall cells.
                            const bool along_x = face == TileFace::North || face == TileFace::South;
                            const std::int32_t t = along_x ? c.x : c.y;
                            const bool upper = (along_x ? along.x : along.y) > 0;
                            straights.push_back({lateral_quarter_turns(face), pending_bottom, pending_count,
                                                 along_x ? c.y : c.x, 2 * t + (upper ? 1 : 0), style,
                                                 pending_piece == TilePiece::WallCapContinue, pending_code});
                        } else {
                            // WallConvex / WallConcave: no horizontal counterpart (each occurs once
                            // per corner tile), so stamp directly with only the vertical stretch.
                            library.append_styled(library.piece(style, pending_piece, orient),
                                                  origin_of(c, pending_bottom), scale, pending_code, false,
                                                  out.vertices, out.indices,
                                                  glm::vec3(1.0f, 1.0f, static_cast<float>(pending_count)));
                        }
                    };

                    for (std::int32_t cell = top_cell; cell >= lowest; --cell) {
                        end = classify_wall_end(pad, c, face, along, cell);
                        const bool      cap   = cell == top_cell;
                        const TileKind  wk    = wall_kind(col, cell);
                        TilePiece       piece = wall_piece(cap, end);
                        const glm::vec3 o     = origin_of(c, cell);
                        // One code per column and tier, so a wall's stamps weld into one surface.
                        float code = cap ? encode_surface_blend(SurfaceBlend::Lip, col.top_kind, wk, wk, wk)
                                         : encode_surface_flat(wk);
                        // An inner corner's two halves belong to two walls. Where the OTHER wall's
                        // plateau ends at this cell but this one keeps rising, this half takes the
                        // taper piece: lipped along the fillet (in the other plateau's turf) to
                        // meet that wall's cap, plain along its own still-rising face.
                        bool lipped = cap;
                        if (end == WallEnd::Concave && !cap) {
                            const TileColumn& partner = pad.at(c + along + step);
                            if (cell == partner.steps - 1) {
                                piece  = TilePiece::WallTaperConcave;
                                code   = encode_surface_blend(SurfaceBlend::Lip, partner.top_kind, wk, wk, wk);
                                lipped = true;
                            }
                        }

                        // A straight cap run that ends against a TALLER neighbour on the same
                        // face: that neighbour's wall is plain at this cell, so this half takes
                        // the continue taper -- its lip easing out to meet that wall square.
                        const bool taper_continue = cap && end == WallEnd::Continue &&
                                                    pad.at(c + along).steps > col.steps;
                        if (taper_continue) piece = TilePiece::WallTaperContinue;

                        // Eligible for the run merge: every plain tier (cap or not) at a straight
                        // end, plus a non-cap corner (Convex/Concave) that is not the one-off
                        // taper cell -- `lipped` is true there and only there among non-cap cells,
                        // since `wall_kind()`'s soil/stone split is the only other thing that can
                        // break a run and that already shows up as a `code` mismatch below.
                        const bool run_eligible = !no_merge && !taper_continue &&
                                                  (end == WallEnd::Continue || (!cap && !lipped));
                        if (run_eligible) {
                            if (pending_active && pending_piece == piece && pending_code == code &&
                                pending_bottom == cell + 1) {
                                pending_bottom = cell;
                                ++pending_count;
                            } else {
                                flush_pending();
                                pending_active = true;
                                pending_piece  = piece;
                                pending_code   = code;
                                pending_bottom = cell;
                                pending_count  = 1;
                            }
                        } else {
                            flush_pending();
                            stamp(library.piece(style, piece, orient), o, code, lipped);
                        }

                        if (have_above && above_end != end) {
                            // Where the end state changes with depth, the shape above is the
                            // smaller one (a convex rounding, or a straight end over a fillet),
                            // so this cell's top section shows as a shelf open to the sky. It is
                            // flush with the neighbouring ground that changed the end -- the
                            // along neighbour under a convex end, the diagonal one under a
                            // fillet -- so it takes that ground's kind, not this wall's.
                            const glm::ivec2 ground = above_end == WallEnd::Convex ? c + along : c + along + step;
                            stamp(library.section(style, piece, Plane::Top, orient), o,
                                  encode_surface_flat(pad.at(ground).top_kind), false);
                            stamp(library.section(style, above_piece, Plane::Bottom, orient),
                                  origin_of(c, cell + 1), above_code, above_cap);
                        }

                        if (end != WallEnd::Convex) {
                            const glm::ivec2 owner = end == WallEnd::Continue ? c + along : c + along + step;
                            const TileFace   owner_face = end == WallEnd::Continue ? face : face_of_step(-along);
                            const bool       next = wall_cell_at(owner, owner_face, cell);
                            // Halves meeting along a wall always agree on tier: a cap meets a cap
                            // at equal heights, and a cap ending against a taller wall's plain cell
                            // is the continue taper, square at that end. At an inner corner both
                            // halves are lipped exactly at the lower plateau's top (cap or taper).
                            // So only their styles can differ.
                            if (next && style_at(owner, pad.at(owner).top_kind) != style) {
                                stamp(library.section(style, piece, Plane::End, orient), o, code, lipped);
                            }
                        }

                        have_above  = true;
                        above_end   = end;
                        above_piece = piece;
                        above_code  = code;
                        above_cap   = lipped;
                    }
                    flush_pending();

                    if (end == WallEnd::Convex) {
                        stamp(library.foot(orient), origin_of(c, lowest), encode_surface_flat(nb.top_kind), false);
                    }
                }
            }
        }
    }

    // --- Interior quadrants, as greedy rectangles of equal height and kind, half a tile wide ---
    // (TOY_TERRAIN_NO_MERGE bypasses this: the loop above then marks nothing in `full`.)
    {
        const std::int32_t n = qn;  // half-tile grid edge
        auto at = [&](std::int32_t x, std::int32_t y) -> std::uint8_t& {
            return full[static_cast<std::size_t>(y * n + x)];
        };
        auto same = [&](std::int32_t ax, std::int32_t ay, std::int32_t bx, std::int32_t by) {
            const TileColumn& a = pad.at(ax / 2, ay / 2);
            const TileColumn& b = pad.at(bx / 2, by / 2);
            return at(bx, by) == 1 && a.steps == b.steps && a.top_kind == b.top_kind;
        };
        // max_merge is in TILES; this grid is half-tiles.
        const std::int32_t most = params.max_merge > 0 ? 2 * params.max_merge : n;
        for (std::int32_t ly = 0; ly < n; ++ly) {
            for (std::int32_t lx = 0; lx < n; ++lx) {
                if (at(lx, ly) != 1) continue;
                std::int32_t w = 1;
                while (lx + w < n && w < most && same(lx, ly, lx + w, ly)) ++w;
                std::int32_t h = 1;
                for (; ly + h < n && h < most; ++h) {
                    bool row = true;
                    for (std::int32_t x = lx; x < lx + w && row; ++x) row = same(lx, ly, x, ly + h);
                    if (!row) break;
                }
                // max_merge == 1 means "never span a tile" (planets: a merged quad bent onto the
                // sphere would cut a chord under the tiles beside it). At half-tile granularity
                // that additionally means never spanning past the tile a run started in.
                if (params.max_merge == 1) {
                    w = std::min(w, 2 - (lx & 1));
                    h = std::min(h, 2 - (ly & 1));
                }
                for (std::int32_t y = ly; y < ly + h; ++y)
                    for (std::int32_t x = lx; x < lx + w; ++x) at(x, y) = 2;
                const TileColumn& col = pad.at(lx / 2, ly / 2);
                const glm::vec3 origin = origin_of({lx / 2, ly / 2}, col.steps - 1) +
                    glm::vec3((lx & 1) ? 0.5f * params.tile_size : 0.0f,
                             (ly & 1) ? 0.5f * params.tile_size : 0.0f, 0.0f);
                library.append_styled(library.full_top(), origin,
                                      glm::vec3(0.5f * scale.x * static_cast<float>(w),
                                               0.5f * scale.y * static_cast<float>(h), scale.z),
                                      encode_surface_flat(col.top_kind), false, out.vertices, out.indices);
            }
        }
    }

    // --- Straight runs ---
    // Sorted into wall lines, then each maximal run of adjacent halves sharing a vertical span,
    // style, tier and kind becomes one strip stretched BOTH ways: horizontally across the halves
    // it absorbs and vertically across the cells each of them already collapsed (see the pending
    // run above). Two halves only merge horizontally if their vertical runs match exactly --
    // `cell_bottom` and `cell_count` both sit in the key -- so a straight stretch can never cut
    // across a boundary a neighbouring column's wall changes height at. The sort key is total, so
    // the output stays deterministic.
    std::sort(straights.begin(), straights.end(), [](const StraightHalf& a, const StraightHalf& b) {
        if (a.turns != b.turns) return a.turns < b.turns;
        if (a.cell_bottom != b.cell_bottom) return a.cell_bottom < b.cell_bottom;
        if (a.cell_count != b.cell_count) return a.cell_count < b.cell_count;
        if (a.line != b.line) return a.line < b.line;
        return a.slot < b.slot;
    });
    for (std::size_t i = 0; i < straights.size();) {
        const StraightHalf& first = straights[i];
        std::size_t j = i + 1;
        // Half-tiles: a run of `max_merge` tiles is twice that many halves.
        const std::size_t most = params.max_merge > 0 ? static_cast<std::size_t>(params.max_merge) * 2 : straights.size();
        while (j < straights.size() && j - i < most) {
            const StraightHalf& h = straights[j];
            if (h.turns != first.turns || h.cell_bottom != first.cell_bottom || h.cell_count != first.cell_count ||
                h.line != first.line || h.slot != first.slot + static_cast<std::int32_t>(j - i) ||
                h.style != first.style || h.cap != first.cap || h.code != first.code) {
                break;
            }
            ++j;
        }
        const bool  along_x = first.turns == 0 || first.turns == 2; // North / South
        const float start   = 0.5f * static_cast<float>(first.slot) * params.tile_size;
        const float across  = static_cast<float>(first.line) * params.tile_size;
        const float h_stretch = 0.5f * static_cast<float>(j - i);
        const glm::vec3 origin(along_x ? start : across, along_x ? across : start,
                               static_cast<float>(first.cell_bottom) * params.height_step);
        const float v_stretch = static_cast<float>(first.cell_count);
        library.append_styled(library.straight(first.style, first.cap, first.turns), origin, scale,
                              first.code, first.cap, out.vertices, out.indices,
                              along_x ? glm::vec3(h_stretch, 1.0f, v_stretch) : glm::vec3(1.0f, h_stretch, v_stretch));
        i = j;
    }

}

namespace {

/**
 * @brief Cell boundaries along one axis of a field level, ascending from 0 to `n`: cell `i`
 *        covers tiles `[out[i], out[i+1])`.
 *
 * Every tile within `border` of the chunk's rim is its own cell, so the rim keeps the terrain's
 * true per-tile heights; the interior is `stride`-tile blocks.
 */
std::vector<std::int32_t> field_cell_edges(std::int32_t n, std::int32_t stride, std::int32_t border) {
    std::vector<std::int32_t> edges;
    edges.push_back(0);
    while (edges.back() < n) {
        const std::int32_t at   = edges.back();
        const std::int32_t next = (at < border || at >= n - border)
                                      ? at + 1
                                      : std::min(at + stride, std::max(at + 1, n - border));
        edges.push_back(std::min(next, n));
    }
    return edges;
}

} // namespace

void mesh_chunk_field(const ColumnPad& pad, const TerrainParams& params, std::int32_t stride,
                      std::int32_t native_border, ChunkMeshData& out) {
    out.clear();
    const std::int32_t n = params.chunk_size;
    if (n <= 0) return;

    const std::int32_t s      = std::max<std::int32_t>(1, stride);
    const std::int32_t border = std::clamp(native_border, 0, n / 2);

    const std::vector<std::int32_t> edges = field_cell_edges(n, s, border);
    const std::int32_t bands = static_cast<std::int32_t>(edges.size()) - 1;
    if (bands <= 0) return;

    // Which cell band a tile falls in, for both axes (a chunk is square).
    std::vector<std::int32_t> band_of(static_cast<std::size_t>(n), 0);
    for (std::int32_t b = 0; b < bands; ++b) {
        for (std::int32_t i = edges[b]; i < edges[b + 1]; ++i) band_of[static_cast<std::size_t>(i)] = b;
    }

    /** @brief One coarse cell: the height it stands at and the kinds it is surfaced in. */
    struct Cell {
        std::int32_t steps     = 0;
        TileKind     top_kind  = TileKind::Grass;
        TileKind     side_kind = TileKind::Dirt;
    };
    std::vector<Cell> cells(static_cast<std::size_t>(bands) * static_cast<std::size_t>(bands));
    for (std::int32_t by = 0; by < bands; ++by) {
        for (std::int32_t bx = 0; bx < bands; ++bx) {
            Cell& cell = cells[static_cast<std::size_t>(by) * static_cast<std::size_t>(bands) +
                               static_cast<std::size_t>(bx)];
            // The TALLEST column the cell covers, not the mean. Two things rest on it: nothing the
            // full mesh carries can poke through a coarse chunk, and -- since every chunk's
            // boundary wall descends to its neighbour's TRUE height (coarse_steps() below) while
            // its own surface only ever sits at or above that height -- two chunks drawing
            // different levels always overlap at the seam instead of opening a hole.
            std::int32_t best = 0;
            for (std::int32_t y = edges[by]; y < edges[by + 1]; ++y) {
                for (std::int32_t x = edges[bx]; x < edges[bx + 1]; ++x) {
                    const TileColumn& col = pad.at(x, y);
                    if (col.steps > best || (x == edges[bx] && y == edges[by])) {
                        best           = col.steps;
                        cell.steps     = col.steps;
                        cell.side_kind = col.side_kind;
                    }
                }
            }
            // The kind the cell reads as is the one most of it is made of, so a lone tall spike
            // raises the cell without also recolouring it.
            std::array<std::uint16_t, k_tile_kind_count> votes{};
            for (std::int32_t y = edges[by]; y < edges[by + 1]; ++y) {
                for (std::int32_t x = edges[bx]; x < edges[bx + 1]; ++x) {
                    ++votes[static_cast<std::size_t>(pad.at(x, y).top_kind)];
                }
            }
            std::size_t winner = 0;
            for (std::size_t k = 1; k < votes.size(); ++k) {
                if (votes[k] > votes[winner]) winner = k;   // ties keep the lower kind: deterministic
            }
            cell.top_kind  = static_cast<TileKind>(winner);
            out.max_steps  = std::max(out.max_steps, cell.steps);
        }
    }

    const float ts = params.tile_size;
    const float hs = params.height_step;

    auto cell_at = [&](std::int32_t x, std::int32_t y) -> const Cell& {
        return cells[static_cast<std::size_t>(band_of[static_cast<std::size_t>(y)]) *
                         static_cast<std::size_t>(bands) +
                     static_cast<std::size_t>(band_of[static_cast<std::size_t>(x)])];
    };
    // Off the chunk this is the neighbour's OWN column, never a coarsened one -- see the Cell
    // comment above for why that is what keeps the seam watertight.
    auto coarse_steps = [&](std::int32_t x, std::int32_t y) -> std::int32_t {
        if (x < 0 || y < 0 || x >= n || y >= n) return pad.at(x, y).steps;
        return cell_at(x, y).steps;
    };

    auto emit_quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d,
                         const glm::vec3& normal, float code) {
        const std::uint32_t base = static_cast<std::uint32_t>(out.vertices.size());
        // The same deterministic tangent choice append_styled() makes, so welding behaves alike.
        const glm::vec3 axis = std::abs(normal.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                         : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec4 tangent(glm::normalize(glm::cross(axis, normal)), 1.0f);
        for (const glm::vec3& p : {a, b, c, d}) {
            coopa::gfx::engine::data::Vertex v;
            v.position = p;
            v.normal   = normal;
            v.uv       = glm::vec2(code, 0.0f);
            v.tangent  = tangent;
            out.vertices.push_back(v);
        }
        for (const std::uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) out.indices.push_back(base + i);
    };

    // --- Tops: one quad per cell ---
    for (std::int32_t by = 0; by < bands; ++by) {
        for (std::int32_t bx = 0; bx < bands; ++bx) {
            const Cell& cell = cells[static_cast<std::size_t>(by) * static_cast<std::size_t>(bands) +
                                     static_cast<std::size_t>(bx)];
            const float x0 = static_cast<float>(edges[bx]) * ts, x1 = static_cast<float>(edges[bx + 1]) * ts;
            const float y0 = static_cast<float>(edges[by]) * ts, y1 = static_cast<float>(edges[by + 1]) * ts;
            const float z  = static_cast<float>(cell.steps) * hs;
            emit_quad({x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}, {0.0f, 0.0f, 1.0f},
                      encode_surface_flat(cell.top_kind));
        }
    }

    // --- Walls: one run per maximal stretch of tile edges sharing both heights and the side kind ---
    // Every wall descends to the neighbour's OWN height, however deep the drop. A coarse cell is
    // the tallest column of a block up to 32 tiles across, so neighbouring cells differ by tens of
    // steps routinely, and a wall stopping short of its neighbour would leave exactly the open
    // band the Cell comment above promises never to open. It costs nothing to run the whole way:
    // a wall here is at most two quads (a soil band and a stone band) whatever its height.
    const std::int32_t soil = std::max(0, params.soil_depth_steps);
    // `u` then `v` is the order that makes the quad face `normal` (bottom edge u->v, then up).
    auto emit_wall = [&](const glm::vec2& u, const glm::vec2& v, std::int32_t bottom, std::int32_t top,
                         const glm::vec3& normal, TileKind side_kind) {
        if (top <= bottom) return;
        const std::int32_t split = std::max(bottom, top - soil);   // soil band at the lip, stone under it
        const auto quad = [&](std::int32_t zb, std::int32_t zt, TileKind kind) {
            if (zt <= zb) return;
            const float fb = static_cast<float>(zb) * hs, ft = static_cast<float>(zt) * hs;
            emit_quad({u.x, u.y, fb}, {v.x, v.y, fb}, {v.x, v.y, ft}, {u.x, u.y, ft}, normal,
                      encode_surface_flat(kind));
        };
        quad(split, top, side_kind);
        quad(bottom, split, TileKind::Stone);
    };

    for (std::int32_t x = 0; x < n; ++x) {
        for (std::int32_t y = 0; y < n;) {
            const std::int32_t own = coarse_steps(x, y), nb = coarse_steps(x + 1, y);
            if (nb >= own) { ++y; continue; }
            const TileKind kind = cell_at(x, y).side_kind;
            std::int32_t   y2   = y + 1;
            while (y2 < n && coarse_steps(x, y2) == own && coarse_steps(x + 1, y2) == nb &&
                   cell_at(x, y2).side_kind == kind) {
                ++y2;
            }
            const float ex = static_cast<float>(x + 1) * ts;
            emit_wall({ex, static_cast<float>(y) * ts}, {ex, static_cast<float>(y2) * ts},
                      nb, own, {1.0f, 0.0f, 0.0f}, kind);
            y = y2;
        }
        for (std::int32_t y = 0; y < n;) {
            const std::int32_t own = coarse_steps(x, y), nb = coarse_steps(x - 1, y);
            if (nb >= own) { ++y; continue; }
            const TileKind kind = cell_at(x, y).side_kind;
            std::int32_t   y2   = y + 1;
            while (y2 < n && coarse_steps(x, y2) == own && coarse_steps(x - 1, y2) == nb &&
                   cell_at(x, y2).side_kind == kind) {
                ++y2;
            }
            const float ex = static_cast<float>(x) * ts;
            emit_wall({ex, static_cast<float>(y2) * ts}, {ex, static_cast<float>(y) * ts},
                      nb, own, {-1.0f, 0.0f, 0.0f}, kind);
            y = y2;
        }
    }
    for (std::int32_t y = 0; y < n; ++y) {
        for (std::int32_t x = 0; x < n;) {
            const std::int32_t own = coarse_steps(x, y), nb = coarse_steps(x, y + 1);
            if (nb >= own) { ++x; continue; }
            const TileKind kind = cell_at(x, y).side_kind;
            std::int32_t   x2   = x + 1;
            while (x2 < n && coarse_steps(x2, y) == own && coarse_steps(x2, y + 1) == nb &&
                   cell_at(x2, y).side_kind == kind) {
                ++x2;
            }
            const float ey = static_cast<float>(y + 1) * ts;
            emit_wall({static_cast<float>(x2) * ts, ey}, {static_cast<float>(x) * ts, ey},
                      nb, own, {0.0f, 1.0f, 0.0f}, kind);
            x = x2;
        }
        for (std::int32_t x = 0; x < n;) {
            const std::int32_t own = coarse_steps(x, y), nb = coarse_steps(x, y - 1);
            if (nb >= own) { ++x; continue; }
            const TileKind kind = cell_at(x, y).side_kind;
            std::int32_t   x2   = x + 1;
            while (x2 < n && coarse_steps(x2, y) == own && coarse_steps(x2, y - 1) == nb &&
                   cell_at(x2, y).side_kind == kind) {
                ++x2;
            }
            const float ey = static_cast<float>(y) * ts;
            emit_wall({static_cast<float>(x) * ts, ey}, {static_cast<float>(x2) * ts, ey},
                      nb, own, {0.0f, -1.0f, 0.0f}, kind);
            x = x2;
        }
    }
}

namespace {

/// Clearance, in tile steps, between a water skirt's outer lip and the ground above it. A tile
/// set rounds a bank's top edge, which lowers its surface near the tile's own border -- exactly
/// where the lip sits. Half a step clears any rounding a one-step bank can have.
constexpr float k_skirt_clearance_steps = 0.5f;

/// Most steps a skirt's outer lip may drop below the water level. Without a cap the lip follows
/// the ground wherever it goes, and a bank one tile wide standing over a sea cliff would hang a
/// sheet of water the whole height of the drop -- hidden inside the cliff's wall, but metres of
/// it. Two steps is under any bank and still inside that wall.
constexpr float k_skirt_max_drop_steps = 2.0f;

} // namespace

void build_chunk_water(const ColumnPad& pad, const TerrainParams& params, ChunkWaterData& still,
                       ChunkWaterData& flowing) {
    still.clear();
    flowing.clear();
    const std::int32_t n = params.chunk_size;
    const float ts = params.tile_size, hs = params.height_step;
    auto surface_z = [&](const TileColumn& c) {
        const float inset = c.water == k_water_flowing ? params.river_inset : params.water_inset;
        return (static_cast<float>(c.water_level) - inset) * hs;
    };
    auto in_pad = [&](std::int32_t x, std::int32_t y) { return x >= -1 && y >= -1 && x <= n && y <= n; };

    // Mean surface height and water depth at tile corner (cx, cy), over the flooded tiles
    // sharing it. Both read the same tiles from the pad, so two chunks assign their shared
    // corners identical values -- the depth feeds the wave attenuation, and a border mismatch
    // there shows as a moving seam. A corner no flooded tile touches (a skirt's far corner,
    // under the bank) is the shoreline: depth 0, and a height that buries it -- see below.
    auto corner_z = [&](std::int32_t cx, std::int32_t cy, float fallback, float& depth, float& wave) {
        float z_sum = 0.0f, d_sum = 0.0f, w_sum = 0.0f;
        float ground = 0.0f;
        bool  have_ground = false;
        int count = 0;
        for (std::int32_t ty = cy - 1; ty <= cy; ++ty) {
            for (std::int32_t tx = cx - 1; tx <= cx; ++tx) {
                if (!in_pad(tx, ty)) continue;
                const TileColumn& c = pad.at(tx, ty);
                const float top = static_cast<float>(c.steps) * hs;
                if (!have_ground || top < ground) {
                    ground = top;
                    have_ground = true;
                }
                if (c.water == k_water_none) continue;
                const float z = surface_z(c);
                z_sum += z;
                d_sum += z - top;
                w_sum += c.water == k_water_flowing ? params.river_wave_scale : 1.0f;
                ++count;
            }
        }
        depth = count > 0 ? std::max(d_sum / static_cast<float>(count), 0.0f) : 0.0f;
        wave = count > 0 ? w_sum / static_cast<float>(count) : 1.0f;
        if (count > 0) return z_sum / static_cast<float>(count);

        // A corner no flooded tile touches is a skirt's OUTER lip, a tile out from the water.
        // The bank it hides under is solid to the water's terrace, but only the tile BESIDE the
        // water is -- the columns this corner actually touches carry no such guarantee, and
        // where a body has just one tile of bank the lip lands on the far side of it, over
        // whatever the ground does next. At the water level it then surfaces again past the
        // bank: the body appears to leak a tile outward. Rounded tiles make it worse, since
        // they lower a bank's surface near the very border the lip sits on.
        //
        // So the lip goes under the GROUND it actually touches, and only as far as that takes
        // it. A bank standing at the water's own terrace keeps its lip AT the water level --
        // the apron stays flat and covers the bank's rounded shoulder, which dips toward the
        // water inside that same tile. Dropping it there instead would undercut the shoulder
        // and leave the soil rim on the top step of the wall showing along every shore. The
        // corners a skirt SHARES with a flooded tile keep the water level regardless, so the
        // surface the player sees is unchanged and only the buried apron slopes.
        const float clearance = k_skirt_clearance_steps * hs;
        const float lip = std::min(fallback, (have_ground ? ground : fallback) - clearance);
        return std::max(lip, fallback - k_skirt_max_drop_steps * hs);
    };

    for (std::int32_t y = 0; y < n; ++y) {
        for (std::int32_t x = 0; x < n; ++x) {
            const TileColumn& c = pad.at(x, y);
            std::uint8_t kind = c.water;
            std::uint8_t flow = c.flow;
            float level = 0.0f;
            if (kind != k_water_none) {
                level = surface_z(c);
            } else {
                // A skirt under a bank: only beside water, at the highest water beside it.
                bool any = false;
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const TileColumn& o = pad.at(x + dx, y + dy);
                        if (o.water == k_water_none) continue;
                        const float z = surface_z(o);
                        if (!any || z > level) level = z;
                        if (!any || o.water == k_water_still) {
                            kind = o.water;
                            flow = o.flow;
                        }
                        any = true;
                    }
                }
                if (!any) continue;
            }
            ChunkWaterData& out = kind == k_water_flowing ? flowing : still;
            glm::vec2 down(1.0f, 0.0f);
            if (kind == k_water_flowing && flow >= 1 && flow <= 8) down = flow_direction(flow);
            const glm::vec2 across(-down.y, down.x);
            const std::uint32_t base = static_cast<std::uint32_t>(out.positions.size());
            const std::int32_t cxs[4] = {x, x + 1, x + 1, x};
            const std::int32_t cys[4] = {y, y, y + 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                float depth = 0.0f, wave = 1.0f;
                const glm::vec3 p(static_cast<float>(cxs[k]) * ts, static_cast<float>(cys[k]) * ts,
                                  corner_z(cxs[k], cys[k], level, depth, wave));
                out.positions.push_back(p);
                out.depths.push_back(depth);
                out.wave_scales.push_back(wave);
                const glm::vec2 xy(p.x, p.y);
                out.uvs.push_back(kind == k_water_flowing ? glm::vec2(glm::dot(xy, down), glm::dot(xy, across)) : xy);
            }
            const std::uint32_t quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            out.indices.insert(out.indices.end(), quad, quad + 6);
        }
    }
}

std::shared_ptr<const coopa::physx::geometry::TriangleMesh> build_chunk_collision(const ChunkMeshData& mesh) {
    if (mesh.empty()) return nullptr;
    std::vector<glm::vec3> positions;
    positions.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) positions.push_back(v.position);
    auto collision = std::make_shared<coopa::physx::geometry::TriangleMesh>(
        coopa::physx::geometry::TriangleMesh::from_triangles(positions, mesh.indices));
    if (collision->empty()) return nullptr;
    return collision;
}

namespace {

/**
 * @brief Creates the job's GPU mesh from its meshed arrays, on the worker that meshed them.
 *
 * See the file doc for why this is safe off the owner thread. Nothing to do for an empty chunk
 * (open sea below the waterline emits no faces) or for a job given no device (the unit tests).
 */
void upload_chunk_mesh(ChunkBuildJob& job) {
    if (job.device == nullptr || job.allocator == nullptr || job.mesh.empty()) return;
    // buffer_count 1: a chunk mesh is written once and never rewritten, unlike a cloth's
    // per-frame ring (see Mesh::from_arrays()'s doc on why that is a count, not a bool).
    job.gpu_mesh = std::make_shared<coopa::gfx::engine::data::Mesh>(coopa::gfx::engine::data::Mesh::from_arrays(
        *job.device, *job.allocator, job.mesh.vertices, job.mesh.indices, 1, false,
        job.lods.empty() ? nullptr : &job.lods));
}

} // namespace

/**
 * @brief Which of a welded chunk's vertices sit on its SEAM with the neighbouring chunks.
 *
 * These are the vertices a level may not move. A chunk picks its level from its own size on
 * screen, so neighbours routinely draw at different levels, and they only meet without a gap
 * if both still have the vertices they shared at full density. meshopt's own border flag is no
 * substitute: it holds the *topological* boundary of the welded surface, and a chunk's edge
 * wall carries that surface over the seam and on down, so the simplifier sees no boundary
 * there at all (measured: 25 seam vertices collapsed anyway, and the reduction stalled).
 *
 * The seam is the chunk's edge in TILE space: `0` and `chunk_size * tile_size` on both axes.
 * This is the flat world's answer; a ChunkShaper has moved the seam by the time locks are
 * wanted, so it supplies its own (ChunkShaper::seam_locks()).
 *
 * @return One flag per vertex, 1 on the seam -- or empty when nothing needs locking.
 */
std::vector<std::uint8_t> tile_seam_locks(const TerrainParams& params, const ChunkMeshData& mesh) {
    const float span = params.chunk_world_size();
    const float eps  = 1e-3f * params.tile_size;
    std::vector<std::uint8_t> locks(mesh.vertices.size(), 0);
    const auto on_edge = [&](float v) { return std::abs(v) < eps || std::abs(v - span) < eps; };
    // Chunk-local already (ChunkMeshData): the chunk spans [0, span] on x and y.
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const glm::vec3& p = mesh.vertices[i].position;
        locks[i] = (on_edge(p.x) || on_edge(p.y)) ? 1u : 0u;
    }
    return locks;
}

/**
 * @brief Appends the chunk's coarser levels to its indices (TerrainParams::chunk_lods).
 *
 * Every level keeps the chunk's seam vertices exactly (tile_seam_locks()), so a chunk at the
 * coarsest level still meets a full-density neighbour on the vertices they share -- which is
 * what lets each chunk choose its own level with no knowledge of the chunks around it.
 *
 * Costs only indices -- every level shares LOD 0's vertices, which are the bulk of a chunk.
 *
 * @param params The terrain's `chunk_lods` table.
 * @param coord  Which chunk, for the shaper's seam test and its shaping.
 * @param mesh   The welded chunk geometry; its `indices` gain each level's range.
 * @return The LOD table for Mesh::from_arrays(), [0] being the full chunk.
 */
void smooth_field_normals(ChunkMeshData& mesh) {
    if (mesh.indices.size() < 3 || mesh.vertices.empty()) return;
    // Positions are chunk-local and land on the tile grid, so quantising is only insurance
    // against two faces computing a shared corner by different expressions.
    struct Key {
        std::int64_t x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = 1469598103934665603ull;
            for (const std::int64_t v : {k.x, k.y, k.z}) {
                h ^= static_cast<std::size_t>(v);
                h *= 1099511628211ull;
            }
            return h;
        }
    };
    const auto key_of = [](const glm::vec3& p) {
        return Key{std::llround(p.x * 4096.0), std::llround(p.y * 4096.0), std::llround(p.z * 4096.0)};
    };

    // Cross products are left UNNORMALISED so each face weighs by its own area: a cell's wide top
    // quad should pull a shared corner further than the sliver of wall beside it.
    std::unordered_map<Key, glm::vec3, KeyHash> sums;
    sums.reserve(mesh.vertices.size());
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const glm::vec3& a = mesh.vertices[mesh.indices[i + 0]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[i + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[i + 2]].position;
        const glm::vec3 face = glm::cross(b - a, c - a);
        for (std::size_t k = 0; k < 3; ++k) sums[key_of(mesh.vertices[mesh.indices[i + k]].position)] += face;
    }
    for (coopa::gfx::engine::data::Vertex& v : mesh.vertices) {
        const auto it = sums.find(key_of(v.position));
        if (it == sums.end()) continue;
        const float len = glm::length(it->second);
        // Faces that cancel -- the two walls of a one-tile ridge against its top -- leave no
        // direction to point. Keeping the flat normal there beats normalising noise.
        if (len < 1e-9f) continue;
        v.normal = it->second / len;
        // The same deterministic tangent rule mesh_chunk_field()'s emit_quad() uses, so the weld
        // after this still merges the corners this pass has just made identical.
        const glm::vec3 axis = std::abs(v.normal.z) < 0.9f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                           : glm::vec3(1.0f, 0.0f, 0.0f);
        v.tangent = glm::vec4(glm::normalize(glm::cross(axis, v.normal)), 1.0f);
    }
}

const TerrainParams::ChunkLod* first_field_lod(const TerrainParams& params) {
    for (const TerrainParams::ChunkLod& lod : params.chunk_lods) {
        if (lod.field_stride > 0) return &lod;
    }
    return nullptr;
}

std::vector<coopa::gfx::engine::data::MeshLod> build_chunk_lods(const TerrainParams& params,
                                                                const ChunkCoord& coord,
                                                                const ColumnPad& pad,
                                                                const TileMeshLibrary& library,
                                                                ChunkMeshData& mesh,
                                                                int skip_field_levels,
                                                                const ChunkShaper* shaper) {
    std::vector<coopa::gfx::engine::data::LodRequest> levels;
    bool any_style = false;
    bool any_field = false;
    for (const TerrainParams::ChunkLod& lod : params.chunk_lods) {
        if (lod.field_stride > 0) { any_field = true; continue; }
        if (lod.style >= 0) { any_style = true; continue; }
        if (lod.ratio <= 0.0f) break;   // a default (ratio 0, no style, no field) entry ends the table
        levels.push_back({lod.ratio, lod.screen_size, 1.0f});
    }
    // TOY_TERRAIN_NO_LODS: diagnostic. Every chunk then draws its base level, which is what
    // separates "the geometry is wrong" from "the COARSE geometry is wrong".
    static const bool no_lods = std::getenv("TOY_TERRAIN_NO_LODS") != nullptr;
    if ((levels.empty() && !any_style && !any_field) || mesh.empty() || no_lods) return {};

    std::vector<coopa::gfx::engine::data::MeshLod> result;
    if (skip_field_levels > 0) {
        // A coarse build: `mesh` already holds the n-th finest `field` level instead of the
        // native geometry, so table[0] is that level, carrying ITS screen_size rather than
        // LOD 0's. No simplification -- a `ratio` level over a quantised field mesh has nothing
        // meaningful to simplify, and its seam locks would be locks on the wrong surface.
        const TerrainParams::ChunkLod* carried = first_field_lod(params);
        coopa::gfx::engine::data::MeshLod base;
        base.first_index = 0;
        base.index_count = static_cast<std::uint32_t>(mesh.indices.size());
        base.vertex_offset = 0;
        base.screen_size = carried != nullptr ? carried->screen_size : 0.0f;
        result.push_back(base);
    } else {
        // Always goes through simplify_levels(), even with an empty `levels`: it is what supplies
        // table[0], the full chunk -- a style-only table (no `ratio` entries) still needs that
        // anchor before the style loop below appends behind it.
        //
        // The seam locks are only read by simplify_levels(), and only when it has `ratio` levels
        // to simplify -- so they are computed inside this branch rather than above it. A planet
        // carries no `ratio` levels at all, and the walk is per-vertex with a planet-space to
        // face-space read-back on every one of them.
        if (!levels.empty()) {
            const std::vector<std::uint8_t> locks = shaper != nullptr
                                                        ? shaper->seam_locks(params, coord, mesh)
                                                        : tile_seam_locks(params, mesh);
            result = coopa::gfx::engine::data::Mesh::simplify_levels(mesh.vertices, mesh.indices,
                                                                     levels, false, &locks);
        } else {
            result = coopa::gfx::engine::data::Mesh::simplify_levels(mesh.vertices, mesh.indices,
                                                                     levels, false, nullptr);
        }
    }
    // Style levels re-mesh from the chunk's own columns and weld independently -- appended
    // behind every ratio level's vertices (simplify_levels() above has already read `ratio`
    // levels' extent off mesh.indices' size at entry, so this order is load-bearing). Skipped on
    // a planet: see build_chunk_lods()'s doc.
    //
    // A `field` level is appended the same way, and unlike a style level it is also built on a
    // planet: it is meshed in face space from the same columns LOD 0 used, so the one thing a
    // re-mesh here cannot otherwise do -- the planet warp -- applies to it unchanged.
    if (any_style || any_field) {
        int fields_seen = 0;
        for (const TerrainParams::ChunkLod& lod : params.chunk_lods) {
            const bool field = lod.field_stride > 0;
            // A coarse build's mesh IS the finest field level, so that one must not be appended
            // behind itself; and a style/ratio level over it is meaningless (see the branch above).
            if (field && skip_field_levels > 0 && fields_seen++ < skip_field_levels) continue;
            if (!field && skip_field_levels > 0) continue;
            if (!field && lod.style < 0) continue;
            // style levels: see build_chunk_lods()'s doc
            if (!field && shaper != nullptr && !shaper->allows_style_lods()) continue;
            ChunkMeshData level_mesh;
            if (field) {
                mesh_chunk_field(pad, params, lod.field_stride, lod.native_border, level_mesh);
                // Before the shaping: in grid space the shared corners are exact grid values,
                // and the shaper carries the smoothed normals onto the world like any others.
                if (lod.smooth) smooth_field_normals(level_mesh);
                if (shaper != nullptr) {
                    // Positions and normals only: a level carries no water of its own.
                    ChunkWaterData none_still, none_flowing;
                    shaper->shape(params, coord, level_mesh, none_still, none_flowing);
                }
            } else {
                mesh_chunk_styled(pad, library, params, level_mesh, lod.style, lod.native_border);
                if (lod.smooth) smooth_field_normals(level_mesh);
            }
            if (level_mesh.empty()) continue;
            coopa::gfx::engine::data::Mesh::weld_and_optimize(level_mesh.vertices, level_mesh.indices);
            coopa::gfx::engine::data::MeshLod out_lod;
            out_lod.first_index   = static_cast<std::uint32_t>(mesh.indices.size());
            out_lod.index_count   = static_cast<std::uint32_t>(level_mesh.indices.size());
            out_lod.vertex_offset = static_cast<std::int32_t>(mesh.vertices.size());
            out_lod.screen_size   = lod.screen_size;
            mesh.vertices.insert(mesh.vertices.end(), level_mesh.vertices.begin(), level_mesh.vertices.end());
            mesh.indices.insert(mesh.indices.end(), level_mesh.indices.begin(), level_mesh.indices.end());
            result.push_back(out_lod);
        }
    }
    return result;
}


std::uint64_t chunk_water_hash(const ChunkWaterData& still, const ChunkWaterData& flowing) {
    if (still.empty() && flowing.empty()) return 0;
    // FNV-1a over the raw vertex data, as the render pipeline's static-caster cache hashes its
    // draws. Collisions would keep a water surface that should have been rebuilt; at 64 bits
    // over data this size that is not a risk worth a byte-wise compare.
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* data, std::size_t bytes) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const auto mix_surface = [&mix](const ChunkWaterData& w) {
        const auto span = [&mix](const auto& v) { mix(v.data(), v.size() * sizeof(v[0])); };
        span(w.positions);
        span(w.uvs);
        span(w.depths);
        span(w.wave_scales);
        span(w.indices);
        mix(&w.frame, sizeof(w.frame));
        mix(&w.local_frame, sizeof(w.local_frame));
    };
    mix_surface(still);
    mix_surface(flowing);
    // A zero hash means "no water": never return it for a chunk that has some.
    return h != 0 ? h : 1ull;
}

void ChunkBuildJob::run() {
    if (library == nullptr) return;
    // ONE build order, whatever the world's shape:
    //
    //   mesh -> water -> shape() -> weld -> collider -> LOD levels -> hash -> upload
    //
    // Water is built from the `pad` rather than from the mesh, which is what lets it sit this
    // early: a coarse and a full build of the same columns then produce the SAME water_hash,
    // and the publish carries the already-baked WaterBody children across a promotion instead
    // of blinking every ocean in the world each time one happens. The shaping has to land
    // between the water and the weld -- the weld, the collider and the simplifier all have to
    // see the geometry where it finally is.
    //
    // A coarse build skips mesh_chunk_styled() entirely -- nearly the whole of a chunk's cost
    // where tiles each emit their own pieces -- and meshes the finest `field` tier in its
    // place. Only worth it where chunks are never evicted (ChunkShaper::allows_coarse_builds);
    // a streaming world drops what leaves view and has no resident far field to pay for. With
    // no `field` tier to stand in, it falls through to a full build rather than drawing nothing.
    const bool want_coarse = coarse_only && !collision &&
                             shaper != nullptr && shaper->allows_coarse_builds();
    const TerrainParams::ChunkLod* coarse = want_coarse ? first_field_lod(params) : nullptr;
    int skip_field_levels = 0;
    if (coarse != nullptr) {
        mesh_chunk_field(pad, params, coarse->field_stride, coarse->native_border, mesh);
        // Before the shaping, as build_chunk_lods() does it: in grid space the shared corners
        // are exact grid values, and the shaper carries the smoothed normals onto the world.
        if (coarse->smooth) smooth_field_normals(mesh);
        skip_field_levels = 1;
    } else {
        mesh_chunk_styled(pad, *library, params, mesh);
    }
    // A collider-only pass wants the ground's shape and nothing else: the chunk's surfaces are
    // already built and drawing.
    if (!collision_only) build_chunk_water(pad, params, water_still, water_flowing);
    if (shaper != nullptr) shaper->shape(params, coord, mesh, water_still, water_flowing);
    // Off the main thread like the meshing itself: the mesher emits every side as its own
    // vertices, so shared corners weld back together here.
    coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
    // Before the LOD levels: the collider is what the chunk is SHAPED like, not what it happens
    // to be drawing this frame, so it is built from LOD 0's indices alone. Its weld, adjacency
    // and BVH are the expensive part -- hence here, on the worker.
    if (collision) collision_mesh = build_chunk_collision(mesh);
    if (collision_only) return;
    lods = build_chunk_lods(params, coord, pad, *library, mesh, skip_field_levels, shaper);
    water_hash = chunk_water_hash(water_still, water_flowing);
    upload_chunk_mesh(*this);
}

void ChunkBuildJob::reset_outputs() {
    mesh.clear();
    lods.clear();
    water_still.clear();
    water_flowing.clear();
    gpu_mesh.reset();
    water_hash = 0;
    collision_mesh.reset();
    library        = nullptr;
    shaper         = nullptr;
    collision      = false;
    collision_only = false;
    coarse_only    = false;
}

glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord,
                       const ChunkShaper* shaper) {
    if (shaper != nullptr) return shaper->chunk_origin(params, coord);
    const float size = params.chunk_world_size();
    return glm::vec3(static_cast<float>(coord.x) * size, static_cast<float>(coord.y) * size, 0.0f);
}

} // namespace world
} // namespace toy
