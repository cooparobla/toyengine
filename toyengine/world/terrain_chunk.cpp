#include <toyengine/world/terrain_chunk.h>

#include <gfxcoopa/engine/data/mesh.h>
#include <physxcoopa/geometry/triangle_mesh.h>
#include <toyengine/world/tile_topology.h>
#include <toyengine/world/tile_types.h>

namespace toy {
namespace world {

void ChunkMeshData::clear() {
    vertices.clear();
    indices.clear();
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

void sample_chunk_columns(const TileMapData& tiles, const TerrainParams& params, const ChunkCoord& coord,
                          ColumnPad& out) {
    out.resize(params.chunk_size);
    const std::int32_t base_x = coord.x * params.chunk_size;
    const std::int32_t base_y = coord.y * params.chunk_size;
    for (std::int32_t ly = -1; ly <= params.chunk_size; ++ly) {
        for (std::int32_t lx = -1; lx <= params.chunk_size; ++lx) {
            out.at(lx, ly) = tiles.get(base_x + lx, base_y + ly).column();
        }
    }
}

void mesh_chunk_styled(const ColumnPad& pad, const TileMeshLibrary& library,
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
    // Whether column `c` has a wall cell on `face` at `cell`.
    auto wall_cell_at = [&](const glm::ivec2& c, TileFace face, std::int32_t cell) {
        const TileColumn& col = pad.at(c);
        const TileColumn& nb  = pad.at(c + face_step(face));
        return cell < col.steps && cell >= nb.steps && cell >= col.steps - params.max_wall_steps;
    };
    auto stamp = [&](const TileMeshLibrary::SideGeometry& g, const glm::vec3& origin, float code,
                     bool anchored) {
        library.append_styled(g, origin, scale, code, anchored, out.vertices, out.indices);
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
        float        code;   // the cell's blend code (encode_surface_blend())
    };
    std::vector<StraightHalf> straights;
    // Tiles with no exposed edge, merged into rectangles after the walk (greedy, row-major):
    // a flat plateau is then a handful of quads, not two triangles a tile.
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
            // A tile set may keep kind boundaries square (a blocky set): then no top is a blend
            // corner, and every quadrant is plain.
            const bool round_corners    = library.rounds_kind_corners(style);
            auto exposed = [&](const glm::ivec2& dir) { return pad.at(c + dir).steps < col.steps; };
            // A neighbour's kind as this top may blend into it: only across a level edge -- across
            // a cliff the two surfaces never touch.
            auto level_kind = [&](const glm::ivec2& dir) {
                const TileColumn& n = pad.at(c + dir);
                return n.steps == col.steps ? n.top_kind : col.top_kind;
            };

            // --- Top ---
            // terrain_styled.frag only acts where a quadrant is a convex corner of its own kind
            // against level neighbours -- the corners it rounds. Anywhere else a top is one kind,
            // so it merges and welds like any plateau interior.
            const bool no_edge = !exposed({0, 1}) && !exposed({0, -1}) && !exposed({1, 0}) && !exposed({-1, 0});
            bool any_corner = false;
            for (std::uint8_t q = 0; q < 4 && round_corners; ++q) {
                const glm::ivec2 sgn = rotate_ccw({1, 0}, q) + rotate_ccw({0, 1}, q);
                any_corner = any_corner || (level_kind({sgn.x, 0}) != col.top_kind &&
                                            level_kind({0, sgn.y}) != col.top_kind);
            }
            if (no_edge && !any_corner) {
                full[static_cast<std::size_t>(ly * params.chunk_size + lx)] = 1;
            } else {
                for (std::uint8_t q = 0; q < 4; ++q) {
                    const glm::ivec2 cy = rotate_ccw({0, 1}, q), cx = rotate_ccw({1, 0}, q);
                    const PiecePlacement place = classify_quadrant(q, exposed(cy), exposed(cx));
                    const glm::ivec2 sgn = cx + cy; // the quadrant's world directions, each +-1
                    const TileKind kx = level_kind({sgn.x, 0}), ky = level_kind({0, sgn.y});
                    const float code = (round_corners && kx != col.top_kind && ky != col.top_kind)
                        ? encode_surface_blend(SurfaceBlend::Top, col.top_kind, kx, ky, level_kind(sgn), sgn.x, sgn.y)
                        : encode_surface_flat(col.top_kind);
                    stamp(library.piece(style, place.piece, place.orientation), origin_of(c, top_cell), code, false);
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
                    float     above_code  = 0.0f;
                    bool      above_cap   = false;
                    WallEnd   end         = WallEnd::Continue;

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

                        if (end == WallEnd::Continue && !taper_continue) {
                            const bool along_x = face == TileFace::North || face == TileFace::South;
                            const std::int32_t t = along_x ? c.x : c.y;
                            const bool upper = (along_x ? along.x : along.y) > 0;
                            straights.push_back({lateral_quarter_turns(face), cell, along_x ? c.y : c.x,
                                                 2 * t + (upper ? 1 : 0), style, cap, code});
                        } else {
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
                            if (next && library.style_of(pad.at(owner).top_kind) != style) {
                                stamp(library.section(style, piece, Plane::End, orient), o, code, lipped);
                            }
                        }

                        have_above  = true;
                        above_end   = end;
                        above_piece = piece;
                        above_code  = code;
                        above_cap   = lipped;
                    }

                    if (lowest == nb.steps && end == WallEnd::Convex) {
                        stamp(library.foot(orient), origin_of(c, lowest), encode_surface_flat(nb.top_kind), false);
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
                                      encode_surface_flat(col.top_kind), false, out.vertices, out.indices);
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
                h.cap != first.cap || h.code != first.code) {
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
                              first.code, first.cap, out.vertices, out.indices,
                              along_x ? 0 : 1, 0.5f * static_cast<float>(j - i));
        i = j;
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

void ChunkBuildJob::run() {
    if (library == nullptr) return;
    mesh_chunk_styled(pad, *library, params, mesh);
    // Off the main thread like the meshing itself: the mesher emits every side as its own
    // vertices, so shared corners weld back together here.
    coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
    // The collider is the drawn surface itself, so it matches whatever tile set built it. Its
    // weld, adjacency and BVH are the expensive part -- hence here, on the worker.
    if (collision) collision_mesh = build_chunk_collision(mesh);
}

glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord) {
    const float size = params.chunk_world_size();
    return glm::vec3(static_cast<float>(coord.x) * size, static_cast<float>(coord.y) * size, 0.0f);
}

} // namespace world
} // namespace toy
