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
using toy::world::TerrainField;
using toy::world::TerrainFieldGeometry;
using toy::world::TerrainMode;
using toy::world::WaterCell;
using toy::world::WaterKind;

namespace {

/** @brief A 16 x 16 one-metre-tile smooth terrain, ground 5 m, chunks of 8 tiles. */
TerrainField smooth() {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Heightmap;
    g.tiles_x = g.tiles_y = 16;
    g.size = glm::vec2(16.0f);
    g.height = 20.0f;
    g.chunk_tiles = 8;
    TerrainField f;
    f.reset(g);
    for (float& h : f.heights()->heights()) h = 5.0f;
    f.mark_all_dirty();
    return f;
}

/** @brief Floods a tile rect to `level` as `kind`, writing the cells directly. */
void flood(TerrainField& f, int x0, int y0, int x1, int y1, float level, WaterKind kind,
           std::uint8_t flow = 0) {
    // The ground under the water has to be below it, or the cell drains on the way in.
    for (int j = 0; j < f.heights()->ny(); ++j) {
        for (int i = 0; i < f.heights()->nx(); ++i) {
            if (i >= x0 && i <= x1 && j >= y0 && j <= y1) f.heights()->at(i, j) = level - 2.0f;
        }
    }
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) f.set_water(glm::ivec2(x, y), WaterCell{kind, level, flow});
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
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, still, flowing);
    expect(still.empty() && flowing.empty(), "dry: no water, no mesh");
}

/** @brief A Tiles field builds nothing here -- build_chunk_water() is its path. */
COOPA_TEST(a_tile_field_is_not_this_builders_job) {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Tiles;
    g.tiles_x = g.tiles_y = 16;
    g.size = glm::vec2(16.0f);
    g.chunk_tiles = 8;
    TerrainField f;
    f.reset(g);
    f.set_water(glm::ivec2(2, 2), WaterCell{WaterKind::Lake, 3.0f, 0});
    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, still, flowing);
    expect(still.empty() && flowing.empty(), "tiles: the smooth builder declines a terraced field");
}

/** @brief A lake becomes a still surface at its level, with depth under each corner. */
COOPA_TEST(a_lake_becomes_a_still_surface) {
    TerrainField f = smooth();
    flood(f, 2, 2, 6, 6, 4.0f, WaterKind::Lake);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, still, flowing);
    require(!still.empty(), "lake: a still mesh was built");
    expect(flowing.empty(), "lake: and nothing flowing");
    expect(still.indices.size() % 3 == 0, "lake: whole triangles");
    expect(still.positions.size() == still.depths.size() &&
               still.positions.size() == still.wave_scales.size() &&
               still.positions.size() == still.uvs.size(),
           "lake: every vertex has a depth, a wave scale and a uv");

    for (const glm::vec3& p : still.positions) {
        expect_near(p.z, 4.0f, 1e-4f, "lake: the surface sits at the level it was given");
    }
    float deepest = 0.0f;
    for (const float d : still.depths) deepest = std::max(deepest, d);
    expect_near(deepest, 2.0f, 1e-4f, "lake: the deepest corner is level minus the bed");
    for (const float w : still.wave_scales) {
        expect_near(w, 1.0f, 1e-4f, "lake: still water waves at full height");
    }
}

/**
 * @brief The surface reaches one tile PAST the water, so the ground is what ends it.
 *
 * That ring is the skirt: without it the surface would stop on a tile boundary instead of at the
 * waterline, and a shore would show a straight edge of water hanging in the air.
 */
COOPA_TEST(the_surface_reaches_past_the_waterline) {
    TerrainField f = smooth();
    flood(f, 2, 2, 4, 4, 4.0f, WaterKind::Lake);

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, still, flowing);
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

/** @brief A river goes to the flowing mesh, with its own wave scale. */
COOPA_TEST(a_river_is_a_separate_flowing_mesh) {
    TerrainField f = smooth();
    flood(f, 2, 2, 6, 4, 4.0f, WaterKind::River, 1);   // flowing east

    ChunkWaterData still, flowing;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, still, flowing);
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
    // A lake straddling the boundary between chunk (0,0) and chunk (1,0) at tile x = 8, with a
    // sloping surface across it so an agreement is a real one rather than one constant value.
    flood(f, 5, 2, 12, 6, 4.0f, WaterKind::Lake);
    for (int y = 2; y < 6; ++y) {
        for (int x = 5; x < 12; ++x) {
            f.set_water(glm::ivec2(x, y), WaterCell{WaterKind::Lake, 4.0f + 0.05f * static_cast<float>(x), 0});
        }
    }
    f.mark_all_dirty();

    ChunkWaterData a_still, a_flow, b_still, b_flow;
    build_smooth_chunk_water(f, glm::ivec2(0, 0), 8, 0.25f, a_still, a_flow);
    build_smooth_chunk_water(f, glm::ivec2(1, 0), 8, 0.25f, b_still, b_flow);
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
