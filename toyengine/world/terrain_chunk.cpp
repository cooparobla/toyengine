#include <toyengine/world/terrain_chunk.h>

#include <cstdlib>
#include <algorithm>
#include <cmath>

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
    if (params.planet) {
        // The skirt past a face's edge is the neighbouring face's tiles, turned (planet.h).
        const PlanetLayout layout = params.layout();
        const std::int32_t face = base_x / layout.n;
        const std::int32_t face_x = base_x - face * layout.n;
        for (std::int32_t ly = -1; ly <= params.chunk_size; ++ly) {
            for (std::int32_t lx = -1; lx <= params.chunk_size; ++lx) {
                const glm::ivec2 t = layout.wrap(face, face_x + lx, base_y + ly);
                out.at(lx, ly) = tiles.get(t.x, t.y).column();
            }
        }
        return;
    }
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
            // (A planet keeps them square too: its shader has no tile grid in world space to round on.)
            const bool round_corners    = library.rounds_kind_corners(style) && !params.planet;
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
            if (!no_merge && no_edge && !any_corner) {
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

                        if (!no_merge && end == WallEnd::Continue && !taper_continue) {
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
                const std::int32_t most = params.max_merge > 0 ? params.max_merge : n;
                std::int32_t w = 1;
                while (lx + w < n && w < most && same(lx, ly, lx + w, ly)) ++w;
                std::int32_t h = 1;
                for (; ly + h < n && h < most; ++h) {
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
        // Half-tiles: a run of `max_merge` tiles is twice that many halves.
        const std::size_t most = params.max_merge > 0 ? static_cast<std::size_t>(params.max_merge) * 2 : straights.size();
        while (j < straights.size() && j - i < most) {
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
 * @brief Bends a planet chunk, meshed in face space, onto its planet (planet.h): every position
 *        through `to_planet()`, every normal through the inverse transpose of that map's
 *        Jacobian (so a wall stays a wall and a top faces out), a fresh tangent, and in `uv.y`
 *        how far each normal faces away from the planet -- the terrain shader's "up" there.
 */
void warp_chunk_to_planet(const TerrainParams& params, const ChunkCoord& coord, ChunkMeshData& mesh,
                          ChunkWaterData& still, ChunkWaterData& flowing) {
    const PlanetLayout layout = params.layout();
    const std::int32_t base_x = coord.x * params.chunk_size;
    const std::int32_t face = base_x / layout.n;
    const double x0 = static_cast<double>(base_x - face * layout.n) * params.tile_size;
    const double y0 = static_cast<double>(coord.y * params.chunk_size) * params.tile_size;
    const double eps = 1e-3 * params.tile_size;
    const auto map = [&](double x, double y, double z) { return layout.to_planet(face, x0 + x, y0 + y, z); };
    for (auto& v : mesh.vertices) {
        const double x = v.position.x, y = v.position.y, z = v.position.z;
        const glm::dvec3 p = map(x, y, z);
        const glm::dvec3 jx = (map(x + eps, y, z) - map(x - eps, y, z)) / (2.0 * eps);
        const glm::dvec3 jy = (map(x, y + eps, z) - map(x, y - eps, z)) / (2.0 * eps);
        const glm::dvec3 jz = (map(x, y, z + eps) - map(x, y, z - eps)) / (2.0 * eps);
        // Normals transform by the cofactor matrix: n' ~ (jy x jz) n.x + (jz x jx) n.y + (jx x jy) n.z.
        const glm::dvec3 n(v.normal);
        glm::dvec3 nn = glm::cross(jy, jz) * n.x + glm::cross(jz, jx) * n.y + glm::cross(jx, jy) * n.z;
        const double len = glm::length(nn);
        nn = len > 0.0 ? nn / len : glm::normalize(p);
        v.position = glm::vec3(p);
        v.normal = glm::vec3(nn);
        const glm::dvec3 axis = std::abs(nn.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
        v.tangent = glm::vec4(glm::vec3(glm::normalize(glm::cross(axis, nn))), 1.0f);
        v.uv.y = static_cast<float>(glm::dot(nn, glm::normalize(p)));
    }
    // The water goes into a frame of its own, local Z out from the planet at the chunk's centre
    // (local X along the face's x there): over a chunk the sphere's surface is still a height
    // field in that frame, which is what the water system works on (WaterBody::local_frame).
    const double half = 0.5 * params.chunk_size * params.tile_size;
    const glm::dvec3 up = glm::normalize(map(half, half, 0.0));
    const glm::dvec3 along = map(half + eps, half, 0.0) - map(half - eps, half, 0.0);
    const glm::dvec3 ax = glm::normalize(along - up * glm::dot(along, up));
    const glm::dvec3 ay = glm::cross(up, ax);
    const glm::dvec3 origin = up * static_cast<double>(layout.radius());
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(glm::vec3(ax), 0.0f);
    frame[1] = glm::vec4(glm::vec3(ay), 0.0f);
    frame[2] = glm::vec4(glm::vec3(up), 0.0f);
    frame[3] = glm::vec4(glm::vec3(origin), 1.0f);
    for (ChunkWaterData* water : {&still, &flowing}) {
        for (glm::vec3& p : water->positions) {
            const glm::dvec3 d = map(p.x, p.y, p.z) - origin;
            p = glm::vec3(glm::dot(d, ax), glm::dot(d, ay), glm::dot(d, up));
        }
        water->frame = frame;
        water->local_frame = true;
    }
}

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
 * A planet's chunk has been bent onto the globe by then, so its four edges are tested as the
 * planes through the planet's centre that they lie in (planet.h) -- see below.
 *
 * @return One flag per vertex, 1 on the seam -- or empty when nothing needs locking.
 */
std::vector<std::uint8_t> chunk_seam_locks(const TerrainParams& params, const ChunkCoord& coord,
                                           const ChunkMeshData& mesh) {
    const float span = params.chunk_world_size();
    const float eps  = 1e-3f * params.tile_size;
    std::vector<std::uint8_t> locks(mesh.vertices.size(), 0);
    const auto on_edge = [&](float v) { return std::abs(v) < eps || std::abs(v - span) < eps; };

    if (!params.planet) {
        // Chunk-local already (ChunkMeshData): the chunk spans [0, span] on x and y.
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const glm::vec3& p = mesh.vertices[i].position;
            locks[i] = (on_edge(p.x) || on_edge(p.y)) ? 1u : 0u;
        }
        return locks;
    }

    // On the globe, a chunk's four edges are each a curve of constant face-space x or y -- and
    // each of those lies in a PLANE through the planet's centre, since every direction along it
    // is a combination of the face's outward axis and one of its own. So the seam test is four
    // dot products, which is both cheaper than reading each vertex back into face space and
    // immune to the thing that breaks it: a vertex sitting exactly on a cube edge resolves to
    // either of the two faces that meet there, and read through the wrong one its face-space
    // coordinate is a whole face away from this chunk's.
    const PlanetLayout layout = params.layout();
    const std::int32_t face = std::clamp(coord.x * params.chunk_size / std::max(1, params.face_tiles), 0, 5);
    const double face_span = static_cast<double>(params.face_tiles) * params.tile_size;
    const double x0 = static_cast<double>(coord.x * params.chunk_size - face * params.face_tiles) * params.tile_size;
    const double y0 = static_cast<double>(coord.y * params.chunk_size) * params.tile_size;
    glm::dvec3 out, ax, ay;
    PlanetLayout::axes(face, out, ax, ay);
    const auto edge_at = [&](double along, bool is_x) {
        const double t = std::tan((2.0 * along / face_span - 1.0) * PlanetLayout::k_pi * 0.25);
        return is_x ? glm::normalize(glm::cross(out + ax * t, ay))
                    : glm::normalize(glm::cross(ax, out + ay * t));
    };
    const glm::dvec3 planes[4] = {edge_at(x0, true), edge_at(x0 + span, true),
                                  edge_at(y0, false), edge_at(y0 + span, false)};
    // The dot product of a unit direction with a unit plane normal is the sine of its angle off
    // the plane, so the tolerance is an ANGLE: a thousandth of a tile, over the planet's radius.
    const double tolerance = static_cast<double>(eps) / std::max(1.0f, layout.radius());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const glm::dvec3 dir = glm::normalize(glm::dvec3(mesh.vertices[i].position));
        for (const glm::dvec3& n : planes) {
            if (std::abs(glm::dot(dir, n)) < tolerance) { locks[i] = 1u; break; }
        }
    }
    return locks;
}

/**
 * @brief Appends the chunk's coarser levels to its indices (TerrainParams::chunk_lods).
 *
 * Every level keeps the chunk's seam vertices exactly (chunk_seam_locks()), so a chunk at the
 * coarsest level still meets a full-density neighbour on the vertices they share -- which is
 * what lets each chunk choose its own level with no knowledge of the chunks around it.
 *
 * Costs only indices -- every level shares LOD 0's vertices, which are the bulk of a chunk.
 *
 * @param params The terrain's `chunk_lods` table.
 * @param coord  Which chunk, for the planet's face-space seam test.
 * @param mesh   The welded chunk geometry; its `indices` gain each level's range.
 * @return The LOD table for Mesh::from_arrays(), [0] being the full chunk.
 */
std::vector<coopa::gfx::engine::data::MeshLod> build_chunk_lods(const TerrainParams& params,
                                                                const ChunkCoord& coord,
                                                                ChunkMeshData& mesh) {
    std::vector<coopa::gfx::engine::data::LodRequest> levels;
    for (const TerrainParams::ChunkLod& lod : params.chunk_lods) {
        if (lod.ratio <= 0.0f) break;   // a zero ratio ends the table
        levels.push_back({lod.ratio, lod.screen_size, 1.0f});
    }
    // TOY_TERRAIN_NO_LODS: diagnostic. Every chunk then draws its base level, which is what
    // separates "the geometry is wrong" from "the COARSE geometry is wrong".
    static const bool no_lods = std::getenv("TOY_TERRAIN_NO_LODS") != nullptr;
    if (levels.empty() || mesh.empty() || no_lods) return {};
    const std::vector<std::uint8_t> locks = chunk_seam_locks(params, coord, mesh);
    return coopa::gfx::engine::data::Mesh::simplify_levels(mesh.vertices, mesh.indices, levels,
                                                           false, &locks);
}


void ChunkBuildJob::run() {
    if (library == nullptr) return;
    mesh_chunk_styled(pad, *library, params, mesh);
    if (params.planet) {
        build_chunk_water(pad, params, water_still, water_flowing);
        warp_chunk_to_planet(params, coord, mesh, water_still, water_flowing);
        coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
        // Before the LOD levels: the collider is what the chunk is SHAPED like, not what it
        // happens to be drawing this frame, so it is built from LOD 0's indices alone.
        if (collision) collision_mesh = build_chunk_collision(mesh);
        lods = build_chunk_lods(params, coord, mesh);
        upload_chunk_mesh(*this);
        return;
    }
    // Off the main thread like the meshing itself: the mesher emits every side as its own
    // vertices, so shared corners weld back together here.
    coopa::gfx::engine::data::Mesh::weld_and_optimize(mesh.vertices, mesh.indices);
    // The collider is the drawn surface itself, so it matches whatever tile set built it. Its
    // weld, adjacency and BVH are the expensive part -- hence here, on the worker. LOD 0's
    // indices, as on the planet path above.
    if (collision) collision_mesh = build_chunk_collision(mesh);
    lods = build_chunk_lods(params, coord, mesh);
    build_chunk_water(pad, params, water_still, water_flowing);
    upload_chunk_mesh(*this);
}

void ChunkBuildJob::reset_outputs() {
    mesh.clear();
    lods.clear();
    water_still.clear();
    water_flowing.clear();
    gpu_mesh.reset();
    collision_mesh.reset();
    library   = nullptr;
    collision = false;
}

glm::vec3 chunk_origin(const TerrainParams& params, const ChunkCoord& coord) {
    if (params.planet) return glm::vec3(0.0f);   // planet chunks are built round the planet's centre
    const float size = params.chunk_world_size();
    return glm::vec3(static_cast<float>(coord.x) * size, static_cast<float>(coord.y) * size, 0.0f);
}

} // namespace world
} // namespace toy
