/**
 * @file terrain_water_mesh_test.cpp
 * @brief The water surface over a smooth terrain's chunks: what it covers, and that two chunks
 *        agree on the edge between them.
 *
 * The seam is the whole risk here. A per-chunk water mesh is where cracks show, and this engine
 * has been bitten by it before (a river and its lake pulling apart at shared vertices). So the
 * test that matters most is not "water appears" but "both chunks wrote the same number for the
 * corner they share".
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <map>
#include <string>

#include <glm/glm.hpp>
#include <toyengine/world/terrain_field.h>
#include <toyengine/world/terrain_water_mesh.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_water_mesh");

using toy::world::build_smooth_chunk_water;
using toy::world::ChunkWaterData;
using toy::world::skirt_tiles_from_metres;
using toy::world::TerraGeometry;
using toy::world::TerrainField;
using toy::world::WaterCell;
using toy::world::WaterKind;

namespace {

// Mirrors terrain_water_mesh.cpp's k_smooth_skirt_clearance (not exported): the tolerance a
// tucked corner is allowed to sit under ground that has risen to meet it.
constexpr float k_smooth_skirt_clearance_test = 0.2f;

// height_sub 2: the minimum that gives a field an actual height plane at all (1 means a terraced
// read of `steps`, no plane). Every helper below reads/writes the plane in METRE-space (dividing
// a sample index by `sub` to recover it) so the tile-space distances and slopes the comments
// describe stay exactly what they say, independent of this choice.
constexpr int k_sub = 2;

/** @brief A 16 x 16 one-metre-tile smooth terrain, ground 5 m, chunks of 8 tiles. */
TerrainField smooth() {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 16;
    g.tile_size = 1.0f;
    g.height = 20.0f;
    g.height_sub = k_sub;
    g.chunk_tiles = 8;
    TerrainField f;
    f.reset(g);
    for (float& h : f.data()->heights()) h = 5.0f;
    f.mark_all_dirty();
    return f;
}

/** @brief Floods a tile rect to `level` as `kind`, writing the cells directly. */
void flood(TerrainField& f, int x0, int y0, int x1, int y1, float level, WaterKind kind,
           std::uint8_t flow = 0) {
    // The ground under the water has to be below it, or the cell drains on the way in.
    for (int j = 0; j < f.data()->height_ny(); ++j) {
        for (int i = 0; i < f.data()->height_nx(); ++i) {
            if (i >= x0 * k_sub && i <= x1 * k_sub && j >= y0 * k_sub && j <= y1 * k_sub) {
                f.data()->height_at(i, j) = level - 2.0f;
            }
        }
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) f.set_water(glm::ivec2(x, y), WaterCell{kind, level, flow});
    }
    f.mark_all_dirty();
}

/**
 * @brief A gentle bed around the lake's 3x3 corner square [4, 6]: ground climbs 0.3 m per METRE
 *        of Chebyshev distance from it, staying under the 4 m level for several rings before the
 *        bulk terrain's 5 m floor would otherwise resume.
 */
void gentle_bed(TerrainField& f) {
    for (int j = 0; j < f.data()->height_ny(); ++j) {
        for (int i = 0; i < f.data()->height_nx(); ++i) {
            const float x_m = static_cast<float>(i) / static_cast<float>(k_sub);
            const float y_m = static_cast<float>(j) / static_cast<float>(k_sub);
            const float dx = std::max({0.0f, 4.0f - x_m, x_m - 6.0f});
            const float dy = std::max({0.0f, 4.0f - y_m, y_m - 6.0f});
            const float dist = std::max(dx, dy);
            if (dist >= 1.0f) f.data()->height_at(i, j) = 2.0f + 0.3f * dist;
        }
    }
    f.mark_all_dirty();
}

/** @brief Every vertex of a mesh, keyed by its xy rounded to a tenth of a millimetre. */
std::map<std::pair<int, int>, float> vertex_heights(const ChunkWaterData& w, glm::vec2 chunk_origin) {
    std::map<std::pair<int, int>, float> out;
    for (const glm::vec3& p : w.positions) {
        const int kx = static_cast<int>(std::lround((p.x + chunk_origin.x) * 10000.0f));
        const int ky = static_cast<int>(std::lround((p.y + chunk_origin.y) * 10000.0f));
        out[{kx, ky}] = p.z;
    }
    return out;
}

} // namespace

/** @brief A dry terrain builds no water at all. */
COOPA_TEST(a_dry_terrain_builds_nothing) {
    const TerrainField f = smooth();
    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 1, still, flowing);
    expect(still.empty() && flowing.empty(), "dry: no water, no mesh");
}

/** @brief A Tiles field builds nothing here -- build_chunk_water() is its path. */
COOPA_TEST(a_tile_field_is_not_this_builders_job) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 16;
    g.tile_size = 1.0f;
    g.chunk_tiles = 8;
    TerrainField f;
    f.reset(g);
    f.set_water(glm::ivec2(2, 2), WaterCell{WaterKind::Lake, 3.0f, 0});
    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 1, still, flowing);
    expect(still.empty() && flowing.empty(), "tiles: the smooth builder declines a terraced field");
}

/** @brief A lake becomes a still surface at its level, with depth under each corner. */
COOPA_TEST(a_lake_becomes_a_still_surface) {
    TerrainField f = smooth();
    flood(f, 2, 2, 6, 6, 4.0f, WaterKind::Lake);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 1, still, flowing);
    require(!still.empty(), "lake: a still mesh was built");
    expect(flowing.empty(), "lake: and nothing flowing");
    expect(still.indices.size() % 3 == 0, "lake: whole triangles");
    expect(still.positions.size() == still.depths.size() &&
               still.positions.size() == still.wave_scales.size() &&
               still.positions.size() == still.uvs.size(),
           "lake: every vertex has a depth, a wave scale and a uv");

    // Corners touching at least one TRULY wet tile keep the exact level -- that is the visible
    // waterline. The flooded tiles are x,y in [2, 6), so every corner in [2, 6] x [2, 6] touches
    // one. Corners further out are the skirt, which this fix deliberately tucks under the ground
    // it crosses rather than holding flat at the level (see `the_skirt_is_tucked_under_the_bank`).
    for (const glm::vec3& p : still.positions) {
        if (p.x >= 2.0f - 1e-4f && p.x <= 6.0f + 1e-4f && p.y >= 2.0f - 1e-4f && p.y <= 6.0f + 1e-4f) {
            expect_near(p.z, 4.0f, 1e-4f, "lake: the surface sits at the level it was given");
        }
    }
    float deepest = 0.0f;
    for (const float d : still.depths) deepest = std::max(deepest, d);
    expect_near(deepest, 2.0f, 1e-4f, "lake: the deepest corner is level minus the bed");
    for (const float w : still.wave_scales) {
        expect_near(w, 1.0f, 1e-4f, "lake: still water waves at full height");
    }
}

/**
 * @brief The surface reaches `skirt_tiles` tiles PAST the water over a cliff, so the ground is
 *        what ends it.
 *
 * That ring is the skirt: without it the surface would stop on a tile boundary instead of at the
 * waterline, and a shore would show a straight edge of water hanging in the air. `smooth()`'s
 * ground outside the lake is a cliff (5 m against a 4 m level), so even a wide `skirt_tiles`
 * stops at one ring here -- the gentle-bed widening is `the_skirt_widens_only_over_a_gentle_bed`.
 */
COOPA_TEST(the_surface_reaches_past_the_waterline) {
    TerrainField f = smooth();
    flood(f, 2, 2, 4, 4, 4.0f, WaterKind::Lake);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 3, still, flowing);
    require(!still.empty(), "skirt: a mesh was built");

    // The wet tiles are x,y in [2, 4); with the skirt ring the surface spans [1, 5] in corners.
    float min_x = 1e9f, max_x = -1e9f;
    for (const glm::vec3& p : still.positions) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
    }
    expect_near(min_x, 1.0f, 1e-4f, "skirt: one tile short of the water on the low side");
    expect_near(max_x, 5.0f, 1e-4f, "skirt: ...and one tile past it on the high side");
}

/**
 * @brief Over a gentle bed the apron reaches as far as `water_skirt_tiles` allows; over a cliff
 *        it still stops at one ring regardless, because widening only continues where the ground
 *        genuinely stays under the water's level (shore_of()'s `d > 1` gate).
 */
COOPA_TEST(the_skirt_widens_only_over_a_gentle_bed) {
    TerrainField f = smooth();
    flood(f, 4, 4, 6, 6, 4.0f, WaterKind::Lake);   // wet tiles x,y in [4, 6)
    gentle_bed(f);

    ChunkWaterData wide_still, wide_flow, narrow_still, narrow_flow;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 3, wide_still, wide_flow);
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 1, narrow_still, narrow_flow);
    require(!wide_still.empty() && !narrow_still.empty(), "widen: both builds made a mesh");

    auto span_x = [](const ChunkWaterData& w) {
        float lo = 1e9f, hi = -1e9f;
        for (const glm::vec3& p : w.positions) { lo = std::min(lo, p.x); hi = std::max(hi, p.x); }
        return hi - lo;
    };
    expect(span_x(wide_still) > span_x(narrow_still) + 1.0f,
           "widen: a larger water_skirt_tiles reaches further over ground that stays underwater");
}

/**
 * @brief A skirt corner never sits above the ground it is tucked under, nor far below the level,
 *        and stays exactly at the level wherever its own ground has not yet risen to meet it.
 */
COOPA_TEST(the_skirt_is_tucked_under_the_bank_it_crosses) {
    TerrainField f = smooth();
    flood(f, 4, 4, 6, 6, 4.0f, WaterKind::Lake);
    gentle_bed(f);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 3, still, flowing);
    require(!still.empty(), "tuck: a mesh was built");

    for (const glm::vec3& p : still.positions) {
        expect(p.z <= 4.0f + 1e-4f, "tuck: a skirt corner never rises above the lake's own level");
        if (p.z >= 4.0f - 1e-4f) continue;   // at the level: nothing to check further
        // Only a corner whose own ground has risen to meet or pass the level is ever tucked
        // below it -- a corner still over genuinely submerged ground stays exactly at the level.
        const float ground = f.ground_at(glm::vec2(p.x, p.y));
        expect(ground >= 4.0f - 1e-4f, "tuck: a corner only ever drops below the level where its "
                                       "own ground has risen to meet it");
    }
}

/**
 * @brief A deep pocket one tile in from the shore must not drag the RIM corner down with it --
 *        the regression test for the "water ramps downward into the bank" defect. The old
 *        `ground_min`-over-four-tile-centres tuck would pull a rim corner by the pocket's own
 *        depth; the fix reads only the ground AT that exact corner.
 */
COOPA_TEST(a_deep_tile_beside_the_shore_does_not_drag_the_rim_down) {
    TerrainField f = smooth();
    flood(f, 4, 4, 6, 6, 4.0f, WaterKind::Lake);   // wet tiles x,y in [4, 6)
    // A steep bank just past the shore (tile 6 and up): ground at 4.5 m, well above the 4 m
    // level, so every corner out there should sit flat at the level. One tile further in, dig a
    // deep pocket (tile 8) well below the level -- it must not reach back and drag the shoreline
    // rim corner (around tile 6-7) down with it.
    for (int j = 0; j < f.data()->height_ny(); ++j) {
        for (int i = 0; i < f.data()->height_nx(); ++i) {
            if (i >= 6 * k_sub && i <= 9 * k_sub && j >= 4 * k_sub && j <= 6 * k_sub) f.data()->height_at(i, j) = 4.5f;
        }
    }
    for (int j = 4 * k_sub; j <= 6 * k_sub; ++j) f.data()->height_at(8 * k_sub, j) = -10.0f;
    f.mark_all_dirty();

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 4, still, flowing);
    require(!still.empty(), "deep-pocket: a mesh was built");

    for (const glm::vec3& p : still.positions) {
        if (p.x < 6.0f - 1e-4f) continue;   // only the bank past the shore, not the open water
        const float ground = f.ground_at(glm::vec2(p.x, p.y));
        if (ground >= 4.0f - 1e-4f) {
            expect_near(p.z, 4.0f, k_smooth_skirt_clearance_test + 1e-4f,
                       "deep-pocket: a corner over ground at/above the level stays close to it, "
                       "never ramped down toward an unrelated deep tile nearby");
        }
    }
}

/**
 * @brief Over a gentle bed the apron reaches all the way to where the ground actually crosses the
 *        waterline -- the regression test for "water doesn't hit the terrain edge". The mesh's
 *        outermost corner must lie at or past the ground's own crossing point, not stop short of
 *        it because the widening gate read a tile's centre instead of its corners.
 */
COOPA_TEST(the_apron_reaches_the_waterline_over_a_gentle_bed) {
    TerrainField f = smooth();
    flood(f, 4, 4, 6, 6, 4.0f, WaterKind::Lake);   // wet tiles x,y in [4, 6)
    // Ground climbs gently along +x only, crossing the 4 m level partway through tile 9 (at
    // x = 9.5): 2.0 m at the lake's edge (x = 6), +0.3 m per corner step after that.
    for (int j = 0; j < f.data()->height_ny(); ++j) {
        for (int i = 0; i < f.data()->height_nx(); ++i) {
            const float x_m = static_cast<float>(i) / static_cast<float>(k_sub);
            if (x_m >= 6.0f) f.data()->height_at(i, j) = 2.0f + 0.3f * (x_m - 6.0f);
        }
    }
    f.mark_all_dirty();

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 4, still, flowing);
    require(!still.empty(), "reach: a mesh was built");

    float max_x = -1e9f;
    for (const glm::vec3& p : still.positions) max_x = std::max(max_x, p.x);
    // The ground crosses 4 m at x = 6 + (4 - 2) / 0.3 = 12.67; the apron must reach at least that
    // far (chunk 0 only spans tiles [0, 8), i.e. corners up to x = 8, so clamp the expectation to
    // the chunk's own extent -- the point is that it is NOT cut short well before even that).
    expect(max_x >= 8.0f - 1e-4f, "reach: the apron runs to the chunk's own edge over a gentle "
                                  "bed rather than stopping a ring early");
}

/** @brief A corner's depth is exactly the level less the ground sampled AT that corner. */
COOPA_TEST(a_corner_carries_the_depth_of_the_ground_under_it) {
    TerrainField f = smooth();
    flood(f, 4, 4, 6, 6, 4.0f, WaterKind::Lake);
    gentle_bed(f);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 3, still, flowing);
    require(still.positions.size() == still.depths.size(), "depth: one depth per vertex");

    for (std::size_t i = 0; i < still.positions.size(); ++i) {
        const glm::vec3& p = still.positions[i];
        const float ground = f.ground_at(glm::vec2(p.x, p.y));
        const float expected = std::max(0.0f, p.z - ground);
        expect_near(still.depths[i], expected, 1e-3f,
                   "depth: depth is the level less the ground sampled at this exact corner");
    }
}

/** @brief A river goes to the flowing mesh, with its own wave scale. */
COOPA_TEST(a_river_is_a_separate_flowing_mesh) {
    TerrainField f = smooth();
    flood(f, 2, 2, 6, 4, 4.0f, WaterKind::River, 1);   // flowing east

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 1, still, flowing);
    require(!flowing.empty(), "river: a flowing mesh was built");
    expect(still.empty(), "river: and nothing still");
    for (const float w : flowing.wave_scales) {
        expect_near(w, 0.25f, 1e-4f, "river: a river's waves are scaled down");
    }
    // `u` runs downstream, so it spreads along the flow and barely at all across it.
    float u_lo = 1e9f, u_hi = -1e9f, v_lo = 1e9f, v_hi = -1e9f;
    for (const glm::vec2& uv : flowing.uvs) {
        u_lo = std::min(u_lo, uv.x); u_hi = std::max(u_hi, uv.x);
        v_lo = std::min(v_lo, uv.y); v_hi = std::max(v_hi, uv.y);
    }
    expect(u_hi - u_lo > v_hi - v_lo, "river: u runs downstream, v across it");
}

/**
 * @brief Two chunks write the SAME height for the corners on the edge between them.
 *
 * The one guarantee that keeps a per-chunk water surface from cracking. Each corner is averaged
 * over the wet tiles touching it, read by GLOBAL tile coordinate, so neither chunk can arrive at
 * a different answer for a corner they share.
 */
COOPA_TEST(neighbouring_chunks_agree_on_their_shared_edge) {
    TerrainField f = smooth();
    // A lake just short of the boundary between chunk (0,0) and chunk (1,0) at tile x = 8, so the
    // SHORE -- the skirt ring, not just open water -- is what straddles it; the skirt is where a
    // ground-aware corner is most likely to disagree between two chunks, so that is what has to
    // be proven, not a corner deep in open water that never consulted the ground at all.
    flood(f, 5, 2, 8, 6, 4.0f, WaterKind::Lake);
    // A visible step per tile of x: with the ramp bug fixed, a corner's level no longer picks up
    // extra slope from a ground-driven tuck (the surrounding ground here is well above every
    // level, so none of these corners are tucked at all) -- the ONLY source of slope left is the
    // water's own level varying by tile, so that has to be large enough on its own to prove this
    // test's point.
    for (int y = 2; y < 6; ++y) {
        for (int x = 5; x < 8; ++x) {
            f.set_water(glm::ivec2(x, y), WaterCell{WaterKind::Lake, 4.0f + 0.3f * static_cast<float>(x), 0});
        }
    }
    f.mark_all_dirty();

    ChunkWaterData a_still, a_flow, b_still, b_flow;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, 2, a_still, a_flow);
    build_smooth_chunk_water(f, glm::ivec2(1, 0), 8, 0.25f, 2, b_still, b_flow);
    require(!a_still.empty() && !b_still.empty(), "seam: both chunks built water");

    const auto a = vertex_heights(a_still, glm::vec2(0.0f, 0.0f));
    const auto b = vertex_heights(b_still, glm::vec2(8.0f, 0.0f));

    int shared = 0;
    for (const auto& [key, height] : a) {
        const auto it = b.find(key);
        if (it == b.end()) continue;
        ++shared;
        expect_near(height, it->second, 1e-5f, "seam: both chunks place a shared corner identically");
    }
    expect(shared >= 4, "seam: the two chunks really do share corners (" + std::to_string(shared) + ")");

    // ...and the surface genuinely slopes, so the agreement above is not trivial.
    float lo = 1e9f, hi = -1e9f;
    for (const auto& [key, height] : a) { lo = std::min(lo, height); hi = std::max(hi, height); }
    expect(hi - lo > 0.1f, "seam: the surface slopes across the chunk, so equal values mean something");
}

/**
 * @brief A corner shared by two DIFFERENT water bodies takes the lower body's level, never a
 *        mean of the two -- `unified_terrain_demo`'s regression: a mean can stand above the
 *        lower body's own shore, which is exactly the "water lying over the terrain" the scene's
 *        heightmap flavour showed at its coastline.
 */
COOPA_TEST(a_shared_corner_between_two_bodies_takes_the_lower_level_not_the_mean) {
    TerrainField f = smooth();
    // Two adjacent still bodies sharing the corner column x = 6: a 6 m body on the low-x side, a
    // 4 m body on the high-x side. Flooding the lower one SECOND leaves the shared corner's
    // ground (flood() sets it to level - 2 over its own rect) low enough for both to stay wet.
    flood(f, 2, 2, 6, 6, 6.0f, WaterKind::Lake);
    flood(f, 6, 2, 10, 6, 4.0f, WaterKind::Lake);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 16, 0.25f, 1, still, flowing);
    require(!still.empty(), "mixed-level: a mesh was built");

    int checked = 0;
    for (const glm::vec3& p : still.positions) {
        if (std::abs(p.x - 6.0f) > 1e-4f) continue;          // the shared corner column
        if (p.y < 2.0f - 1e-4f || p.y > 6.0f + 1e-4f) continue;
        ++checked;
        expect_near(p.z, 4.0f, 1e-4f,
                    "mixed-level: the shared corner sits at the lower body's 4 m, not the mean 5 m");
    }
    expect(checked > 0, "mixed-level: at least one shared corner was actually on the shared edge");
}

/**
 * @brief `skirt_tiles_from_metres()` keeps the apron's physical reach constant across tile sizes
 *        -- the fix for `unified_terrain_demo`'s heightmap apron silently reaching twice as far
 *        as intended once its tiles were 2 m instead of the tile flavours' 1 m.
 */
COOPA_TEST(skirt_tiles_from_metres_keeps_the_same_physical_reach) {
    expect(skirt_tiles_from_metres(4.0f, 1.0f) == 4, "skirt-metres: 4 m at 1 m tiles is 4 tiles");
    expect(skirt_tiles_from_metres(4.0f, 2.0f) == 2,
           "skirt-metres: 4 m at 2 m tiles is 2 tiles -- half as many, the same physical reach");
    expect(skirt_tiles_from_metres(4.0f, 0.5f) == 8, "skirt-metres: 4 m at 0.5 m tiles is 8 tiles");
    expect(skirt_tiles_from_metres(0.1f, 2.0f) == 1, "skirt-metres: never rounds below one tile");
}

/**
 * @brief A river whose level falls continuously down a slope makes a surface that falls with it,
 *        staying over its own bed the whole way.
 *
 * `unified_terrain_demo`'s heightmap flavour showed river water cutting through steep ground
 * because the bake rounded every level to a whole terrace: a flat sheet per step, over ground
 * that keeps falling, buries its uphill edge and floats its downhill one. The mesher was always
 * able to follow a falling level -- this is the lock on that, so the surface is only ever as
 * stepped as the levels handed to it.
 */
COOPA_TEST(a_river_falling_down_a_slope_keeps_its_surface_over_its_bed) {
    TerrainField f = smooth();
    // Ground falling half a metre per metre of x, with a metre-deep channel cut along y = [6, 10)
    // and the river's surface 0.4 m under the bank beside it -- the shape mapcoopa bakes, read at
    // whatever fraction of a step it lands on rather than rounded to one.
    const auto bank_at = [](float x_m) { return 12.0f - 0.5f * x_m; };
    const auto level_at = [&](int tx) { return bank_at(static_cast<float>(tx) + 0.5f) - 0.4f; };
    for (int j = 0; j < f.data()->height_ny(); ++j) {
        for (int i = 0; i < f.data()->height_nx(); ++i) {
            const float x_m = static_cast<float>(i) / static_cast<float>(k_sub);
            const float y_m = static_cast<float>(j) / static_cast<float>(k_sub);
            const bool in_channel = y_m >= 6.0f && y_m <= 10.0f;
            f.data()->height_at(i, j) = bank_at(x_m) - (in_channel ? 1.0f : 0.0f);
        }
    }
    for (int y = 6; y < 10; ++y) {
        for (int x = 2; x < 14; ++x) {
            f.set_water(glm::ivec2(x, y), WaterCell{WaterKind::River, level_at(x), 3});
        }
    }
    f.mark_all_dirty();

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 16, 0.25f, 1, still, flowing);
    require(!flowing.empty(), "river-slope: the river meshed");

    // Corners with a wet tile on every side: the open channel, where the surface is the level and
    // nothing has been tucked under a bank.
    int interior = 0;
    for (std::size_t i = 0; i < flowing.positions.size(); ++i) {
        const glm::vec3& p = flowing.positions[i];
        const int cx = static_cast<int>(std::lround(p.x)), cy = static_cast<int>(std::lround(p.y));
        if (cx < 3 || cx > 13 || cy < 7 || cy > 9) continue;
        ++interior;
        // The lowest level over a corner is the downstream tile's, cx -- the mesher's own rule.
        expect_near(p.z, level_at(cx), 1e-4f, "river-slope: the surface stands at its own level");
        const float bed = bank_at(p.x) - 1.0f;
        expect(p.z > bed, "river-slope: the surface stays over the bed it runs in");
        expect_near(flowing.depths[i], level_at(cx) - bed, 1e-3f,
                    "river-slope: depth is the level over the bed at that exact corner");
    }
    expect(interior > 20, "river-slope: the open channel really was sampled (" + std::to_string(interior) + ")");

    // And it genuinely falls, by the half metre per tile the levels describe -- not in terraces.
    const auto heights = vertex_heights(flowing, glm::vec2(0.0f, 0.0f));
    float lo = 1e9f, hi = -1e9f;
    for (const auto& [key, height] : heights) { lo = std::min(lo, height); hi = std::max(hi, height); }
    expect(hi - lo > 4.0f, "river-slope: the surface falls across the run, rather than sitting flat");
}

/**
 * @brief The apron stops where the ground falls away beneath the water instead of running up to
 *        meet it -- `unified_terrain_demo`'s heightmap rivers draping sheets down a hillside.
 *
 * The widening gate asks whether a tile's ground is still under the water's level. A shelf
 * running into a lake satisfies that; so does a cliff under water perched above it, and there
 * the apron has left the body's basin. A render of the demo with the apron cut to one tile is
 * what identified this: the shards on its mountainsides went with it.
 */
COOPA_TEST(the_apron_stops_where_the_ground_falls_out_of_the_basin) {
    TerrainField f = smooth();
    // A 2-tile-wide body at 4 m with a bed at 2 m, and ground that FALLS steeply away on its +x
    // side -- every tile over there is "under the level", and none of it is the body's basin.
    flood(f, 4, 4, 6, 8, 4.0f, WaterKind::Lake);
    for (int j = 0; j < f.data()->height_ny(); ++j) {
        for (int i = 0; i < f.data()->height_nx(); ++i) {
            const float x_m = static_cast<float>(i) / static_cast<float>(k_sub);
            if (x_m > 6.0f) f.data()->height_at(i, j) = 2.0f - 3.0f * (x_m - 6.0f);
        }
    }
    f.mark_all_dirty();

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 16, 0.25f, 4, still, flowing);
    require(!still.empty(), "basin: a mesh was built");

    float max_x = -1e9f;
    for (const glm::vec3& p : still.positions) max_x = std::max(max_x, p.x);
    // The wet tiles end at x = 6; one ring of apron covers the quantised waterline, and the
    // fall-away stops the rest -- not the four tiles `skirt_tiles` would otherwise allow.
    expect_near(max_x, 7.0f, 1e-4f,
                "basin: the apron stops one tile past the water, not four, over ground falling away");
}

/**
 * @brief A channel's apron reaches one tile however wide the authored one is; still water on the
 *        same bed still gets its full reach.
 */
COOPA_TEST(a_flowing_body_keeps_a_short_apron) {
    const auto reach_x = [](WaterKind kind) {
        TerrainField f = smooth();
        flood(f, 4, 4, 6, 6, 4.0f, kind, kind == WaterKind::River ? 1 : 0);
        gentle_bed(f);   // a shelf that genuinely stays under the level for several rings
        ChunkWaterData still, flowing;
        build_smooth_chunk_water(f, glm::ivec2(0, 0), 16, 0.25f, 3, still, flowing);
        const ChunkWaterData& w = kind == WaterKind::River ? flowing : still;
        float hi = -1e9f;
        for (const glm::vec3& p : w.positions) hi = std::max(hi, p.x);
        return hi;
    };
    // The wet tiles end at x = 6 either way; only the apron past that differs.
    expect_near(reach_x(WaterKind::River), 7.0f, 1e-4f, "flow-apron: a river reaches one tile past its bank");
    expect(reach_x(WaterKind::Lake) > 7.0f + 1e-4f,
           "flow-apron: still water on the same gentle bed still reaches further");
}
