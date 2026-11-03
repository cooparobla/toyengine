/**
 * @file terrain_water_test.cpp
 * @brief The water operations over a terrain field, device-free: the same flood, drain and
 *        settle asked of a terraced tile world and of a smooth heightmap.
 *
 * Water is a kind plus a surface level, and depth is `level - ground`. The claim under test is
 * that this makes one set of operations correct in both modes -- so every test here runs the
 * same scenario twice and holds both answers to the same contract.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>

#include <glm/glm.hpp>
#include <toyengine/world/terrain_field.h>
#include <toyengine/world/terrain_water.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_water");

using toy::world::borrow_tiles;
using toy::world::IRect;
using toy::world::k_water_flood_cap;
using toy::world::ShoreGrade;
using toy::world::TerrainField;
using toy::world::TerrainFieldGeometry;
using toy::world::TerrainMode;
using toy::world::TileRecord;
using toy::world::WaterCell;
using toy::world::WaterKind;
using toy::world::WaterSettle;

namespace {

/** @brief How many tiles of a field are wet. */
int wet_count(const TerrainField& f) {
    int n = 0;
    for (int y = 0; y < f.geometry().tiles_y; ++y) {
        for (int x = 0; x < f.geometry().tiles_x; ++x) {
            if (f.water_at(glm::ivec2(x, y)).wet()) ++n;
        }
    }
    return n;
}

/**
 * @brief 9 x 9 one-metre tiles, ground 5 steps all round, a 3 x 3 pit of 2 in the middle and one
 *        notch of 3 leading east -- a basin with a spillway.
 */
TerrainField tiled_basin() {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Tiles;
    g.tiles_x = g.tiles_y = 9;
    g.size = glm::vec2(9.0f);
    g.height_step = 1.0f;
    TerrainField f;
    f.reset(g);
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x) {
            f.tiles()->at(x, y).steps = (x >= 3 && x <= 5 && y >= 3 && y <= 5) ? 2 : 5;
        }
    }
    f.tiles()->at(6, 4).steps = 3;
    f.mark_all_dirty();
    return f;
}

/**
 * @brief 8 x 8 two-metre tiles over a 17 x 17 sample plane, ground 5 m with a smooth bowl of 2 m.
 *
 * The samples in [3, 6] are 2 m, so the nine tiles whose four corners all lie inside that block
 * read exactly 2 m and everything around them reads higher -- a bowl with a sloped rim rather
 * than a terrace with a wall.
 */
TerrainField smooth_basin() {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Heightmap;
    g.tiles_x = g.tiles_y = 8;
    g.size = glm::vec2(16.0f);
    g.height = 20.0f;
    g.height_sub = 1;
    g.layer_sub = 1;
    TerrainField f;
    f.reset(g);
    toy::world::HeightmapData& h = *f.heights();
    for (int j = 0; j < h.ny(); ++j) {
        for (int i = 0; i < h.nx(); ++i) {
            const bool in_bowl = i >= 3 && i <= 6 && j >= 3 && j <= 6;
            h.at(i, j) = in_bowl ? 2.0f : 5.0f;
        }
    }
    f.mark_all_dirty();
    return f;
}

} // namespace

/** @brief A flood fills the basin it is seeded in and stops where the ground holds it. */
COOPA_TEST(a_flood_fills_its_basin_in_both_modes) {
    TerrainField t = tiled_basin();
    const IRect flooded = flood_water(t, {{4, 4}}, 4.0f, WaterKind::Lake);
    expect(wet_count(t) == 10, "tiles: the 3 x 3 pit and its notch, nothing further");
    expect(t.water_at(glm::ivec2(6, 4)).kind == WaterKind::Lake, "tiles: the notch took the lake");
    expect_near(t.water_at(glm::ivec2(6, 4)).level, 4.0f, 1e-5f, "tiles: ...at the level it was given");
    expect(flooded.x0 == 3 && flooded.x1 == 7, "tiles: and the rect it changed is reported");
    expect(!t.water_at(glm::ivec2(0, 0)).wet(), "tiles: ground above the level stays dry");

    TerrainField s = smooth_basin();
    flood_water(s, {{4, 4}}, 2.5f, WaterKind::Lake);
    expect(wet_count(s) == 9, "heightmap: the nine tiles of the bowl floor");
    expect_near(s.water_at(glm::ivec2(4, 4)).level, 2.5f, 1e-5f,
                "heightmap: the level is kept exactly, not snapped to a step");
    expect(!s.water_at(glm::ivec2(2, 2)).wet(), "heightmap: the sloped rim is above the level, so dry");
}

/** @brief Water never stands at or under the ground: a level that does not clear it does nothing. */
COOPA_TEST(water_never_stands_on_dry_ground) {
    TerrainField t = tiled_basin();
    flood_water(t, {{0, 0}}, 5.0f, WaterKind::Lake);   // the rim's own height
    expect(wet_count(t) == 0, "tiles: a level level with the ground floods nothing");

    TerrainField s = smooth_basin();
    flood_water(s, {{0, 0}}, 5.0f, WaterKind::Lake);
    expect(wet_count(s) == 0, "heightmap: likewise on a slope");
}

/** @brief A spread carries what the water IS -- its kind, and a river's direction. */
COOPA_TEST(a_spread_carries_the_kind_and_the_flow) {
    TerrainField t = tiled_basin();
    // A river at level 4 on the basin floor, flowing east, and a channel dug beside it.
    t.set_water(glm::ivec2(3, 4), WaterCell{WaterKind::River, 4.0f, 3});
    t.tiles()->at(4, 4).steps = 1;
    t.mark_tiles_dirty(IRect{4, 4, 5, 5});
    settle_water(t, IRect{4, 4, 5, 5});

    const WaterCell filled = t.water_at(glm::ivec2(4, 4));
    expect(filled.kind == WaterKind::River, "tiles: the dug tile fills as river water");
    expect(filled.flow == 3, "tiles: ...flowing the same way");
    expect_near(filled.level, 4.0f, 1e-5f, "tiles: ...at the river's level");

    // Ocean and Lake are different kinds, and a spread keeps them apart.
    TerrainField o = tiled_basin();
    o.set_water(glm::ivec2(3, 3), WaterCell{WaterKind::Ocean, 4.0f, 0});
    o.tiles()->at(4, 3).steps = 1;
    o.mark_tiles_dirty(IRect{4, 3, 5, 4});
    settle_water(o, IRect{4, 3, 5, 4});
    expect(o.water_at(glm::ivec2(4, 3)).kind == WaterKind::Ocean, "tiles: the sea spreads as the sea");
    expect(o.water_at(glm::ivec2(4, 3)).flow == 0, "tiles: ...and still water carries no flow");

    TerrainField s = smooth_basin();
    s.set_water(glm::ivec2(3, 3), WaterCell{WaterKind::River, 2.5f, 7});
    s.mark_tiles_dirty(IRect{3, 3, 4, 4});
    settle_water(s, IRect{3, 3, 4, 4});
    const WaterCell sw = s.water_at(glm::ivec2(4, 4));
    expect(sw.kind == WaterKind::River && sw.flow == 7, "heightmap: a river spreads as a river too");
}

/**
 * @brief Ground raised through its own surface becomes dry land; the water round it stays.
 *
 * The two modes get there differently, and the difference is the point of the field's own water
 * level. In `Tiles` a record's surface IS `steps + water_depth`, so ground above its own surface
 * cannot be written down: raising a bed re-applies the water at its old level, which drains that
 * tile on the spot (`move_ground()` in tile_edit.cpp), and the settle's job is only the
 * neighbours. In `Heightmap` the ground plane and the water level are independent, so the state
 * IS representable -- and removing it is work only the settle can do.
 */
COOPA_TEST(ground_raised_through_the_surface_dries) {
    TerrainField t = tiled_basin();
    flood_water(t, {{4, 4}}, 4.0f, WaterKind::Lake);
    {
        // What a brush does: move the bed, keep the water's surface where it was.
        TileRecord& bed = t.tiles()->at(4, 4);
        const int level = bed.water_level();
        bed.steps = 6;
        bed.set_water(level);
    }
    t.mark_tiles_dirty(IRect{4, 4, 5, 5});
    settle_water(t, IRect{4, 4, 5, 5});
    expect(!t.water_at(glm::ivec2(4, 4)).wet(), "tiles: a bed raised through the water is dry land");
    expect(t.water_at(glm::ivec2(3, 4)).wet() && t.water_at(glm::ivec2(5, 4)).wet(),
           "tiles: ...and the water around it stays");

    TerrainField s = smooth_basin();
    flood_water(s, {{4, 4}}, 2.5f, WaterKind::Lake);
    toy::world::HeightmapData& h = *s.heights();
    for (int j = 4; j <= 5; ++j) {
        for (int i = 4; i <= 5; ++i) h.at(i, j) = 6.0f;
    }
    s.mark_tiles_dirty(IRect{4, 4, 5, 5});
    expect(s.water_at(glm::ivec2(4, 4)).wet(),
           "heightmap: the ground is above the level and the water is still recorded");
    settle_water(s, IRect{4, 4, 5, 5});
    expect(!s.water_at(glm::ivec2(4, 4)).wet(), "heightmap: ...and the settle is what removes it");
    expect(s.water_at(glm::ivec2(3, 4)).wet(), "heightmap: the water beside it stays");
}

/** @brief A drain takes the whole connected body, at whatever level it stands. */
COOPA_TEST(a_drain_takes_the_whole_body) {
    TerrainField t = tiled_basin();
    flood_water(t, {{4, 4}}, 4.0f, WaterKind::Lake);
    expect(wet_count(t) > 0, "tiles: there is water to drain");
    drain_water(t, {{3, 3}});
    expect(wet_count(t) == 0, "tiles: drain takes the whole body");

    TerrainField s = smooth_basin();
    flood_water(s, {{4, 4}}, 2.5f, WaterKind::Lake);
    expect(wet_count(s) > 0, "heightmap: there is water to drain");
    drain_water(s, {{4, 4}});
    expect(wet_count(s) == 0, "heightmap: drain takes the whole body");
}

/**
 * @brief A settle taken a few tiles at a time ends exactly where the all-at-once one does.
 *
 * The contract gameplay editing rests on: TerrainSystem spends a bounded number of tiles a frame
 * on a flood (TerrainComponent::water_settle_budget), so digging beside the sea cannot cost one
 * frame the whole basin. The budget may change WHEN the water arrives, never where it ends up.
 * `tile_map_test` holds this for a tile world; this is the smooth one.
 */
COOPA_TEST(a_budgeted_settle_matches_one_call_on_a_smooth_field) {
    // A long trough one tile wide, with water at one end: a flood with a lot of tiles to cross.
    const auto build = [] {
        TerrainFieldGeometry g;
        g.mode = TerrainMode::Heightmap;
        g.tiles_x = g.tiles_y = 32;
        g.size = glm::vec2(32.0f);
        g.height = 20.0f;
        TerrainField f;
        f.reset(g);
        toy::world::HeightmapData& h = *f.heights();
        for (int j = 0; j < h.ny(); ++j) {
            for (int i = 0; i < h.nx(); ++i) h.at(i, j) = (j >= 15 && j <= 17) ? 1.0f : 6.0f;
        }
        f.set_water(glm::ivec2(0, 16), WaterCell{WaterKind::Ocean, 4.0f, 0});
        f.mark_all_dirty();
        return f;
    };

    TerrainField whole = build();
    settle_water(whole, IRect{0, 16, 1, 17});

    TerrainField stepped = build();
    WaterSettle settle = begin_settle(stepped, IRect{0, 16, 1, 17});
    int rounds = 0;
    while (!settle.done() && rounds < 1000) {
        step_settle(stepped, settle, 3);   // three tiles a "frame"
        ++rounds;
    }
    expect(rounds > 3, "settle: the trough took several steps to fill (" + std::to_string(rounds) + ")");
    expect(settle.done(), "settle: ...and then finished");
    expect(wet_count(whole) > 20, "settle: the one-shot call filled the trough");

    bool same = wet_count(whole) == wet_count(stepped);
    for (int y = 0; y < 32 && same; ++y) {
        for (int x = 0; x < 32; ++x) {
            const WaterCell a = whole.water_at(glm::ivec2(x, y));
            const WaterCell b = stepped.water_at(glm::ivec2(x, y));
            if (a.kind != b.kind || std::abs(a.level - b.level) > 1e-5f) { same = false; break; }
        }
    }
    expect(same, "settle: a budgeted settle reaches exactly the same water as one call");
}

namespace {

/** @brief A flat 9 x 9 corner plane (8 x 8 tiles) at `ground`, with a square lake wet at `level`. */
std::vector<float> flat_plane_with_lake(int n, float ground, float level, std::vector<WaterCell>& water,
                                        int lake_x0, int lake_y0, int lake_x1, int lake_y1) {
    std::vector<float> h(static_cast<std::size_t>(n) * n, ground);
    water.assign(static_cast<std::size_t>(n - 1) * (n - 1), WaterCell{});
    for (int y = lake_y0; y < lake_y1; ++y) {
        for (int x = lake_x0; x < lake_x1; ++x) water[static_cast<std::size_t>(y) * (n - 1) + x] = WaterCell{WaterKind::Lake, level, 0};
    }
    return h;
}

/** @brief The mean of a tile's four corners -- the same bilinear value TerrainField::set_water() reads. */
float tile_mean(const std::vector<float>& h, int n, int x, int y) {
    return 0.25f * (h[static_cast<std::size_t>(y) * n + x] + h[static_cast<std::size_t>(y) * n + x + 1] +
                    h[static_cast<std::size_t>(y + 1) * n + x] + h[static_cast<std::size_t>(y + 1) * n + x + 1]);
}

} // namespace

/** @brief Every wet tile's four-corner mean ends at least `clearance` under its own level. */
COOPA_TEST(grade_water_shore_clears_the_sampling_mismatch) {
    constexpr int n = 9;   // 8 x 8 tiles
    std::vector<WaterCell> water;
    std::vector<float> h = flat_plane_with_lake(n, 4.0f, 4.0f, water, 2, 2, 6, 6);   // flat at the level: worst case

    grade_water_shore(h, n, n, water, n - 1, n - 1, ShoreGrade{0.03f, 0.0f, 4.0f});

    for (int y = 2; y < 6; ++y) {
        for (int x = 2; x < 6; ++x) {
            expect(tile_mean(h, n, x, y) <= 4.0f - 0.03f + 1e-5f,
                   "grade: a wet tile's corner mean clears the water's own dryness test");
        }
    }
}

/** @brief Grading only ever lowers a sample -- it can never flood ground no tile called wet. */
COOPA_TEST(grade_water_shore_never_raises_a_sample) {
    constexpr int n = 9;
    std::vector<WaterCell> water;
    std::vector<float> h = flat_plane_with_lake(n, 4.0f, 4.0f, water, 2, 2, 6, 6);
    const std::vector<float> before = h;

    grade_water_shore(h, n, n, water, n - 1, n - 1, ShoreGrade{0.03f, 1.0f, 3.0f});   // with the ramp too

    for (std::size_t i = 0; i < h.size(); ++i) {
        expect(h[i] <= before[i] + 1e-6f, "grade: no sample is ever raised");
    }
}

/**
 * @brief A bed mapcoopa already graded is left as it was (within a millimetre) when `depth == 0`.
 *
 * The regression test for the old binary clamp: that clamp flattened a hand-graded shelf to a
 * constant pan, where the mean-deficit fix should find nothing left to do.
 */
COOPA_TEST(grade_water_shore_leaves_a_pregraded_shelf_alone) {
    constexpr int n = 9;
    std::vector<WaterCell> water;
    // The lake's bed is already 0.5 m under its level -- comfortably past the 0.03 m clearance --
    // and the shore outside it ramps smoothly up to the 4.0 m rim, exactly what a real bake does.
    std::vector<float> h(static_cast<std::size_t>(n) * n);
    water.assign(static_cast<std::size_t>(n - 1) * (n - 1), WaterCell{});
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int dist = std::max({0, 2 - x, x - 6, 2 - y, y - 6});
            h[static_cast<std::size_t>(y) * n + x] = dist == 0 ? 3.5f : std::min(4.0f, 3.5f + 0.5f * static_cast<float>(dist));
        }
    }
    for (int y = 2; y < 6; ++y) {
        for (int x = 2; x < 6; ++x) water[static_cast<std::size_t>(y) * (n - 1) + x] = WaterCell{WaterKind::Lake, 4.0f, 0};
    }
    const std::vector<float> before = h;

    grade_water_shore(h, n, n, water, n - 1, n - 1, ShoreGrade{0.03f, 0.0f, 4.0f});

    for (std::size_t i = 0; i < h.size(); ++i) {
        expect_near(h[i], before[i], 1e-3f, "grade: an already-graded shelf is left alone, not planed flat");
    }
}

/** @brief The optional ramp adds depth gradually -- it never opens a vertical wall between neighbours. */
COOPA_TEST(grade_water_shore_ramp_introduces_no_cliff) {
    constexpr int n = 17;   // 16 x 16 tiles: room for the ramp's band to run
    std::vector<WaterCell> water;
    std::vector<float> h = flat_plane_with_lake(n, 4.0f, 4.0f, water, 6, 6, 10, 10);

    const ShoreGrade grade{0.03f, 1.5f, 4.0f};
    grade_water_shore(h, n, n, water, n - 1, n - 1, grade);

    float max_step = 0.0f;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n - 1; ++x) {
            max_step = std::max(max_step, std::abs(h[static_cast<std::size_t>(y) * n + x + 1] - h[static_cast<std::size_t>(y) * n + x]));
        }
    }
    expect(max_step <= grade.depth, "grade: the ramp never steps between neighbours by more than its own depth");
}

/** @brief Running the grade twice changes nothing the second time. */
COOPA_TEST(grade_water_shore_is_idempotent) {
    constexpr int n = 13;
    std::vector<WaterCell> water;
    std::vector<float> h = flat_plane_with_lake(n, 4.0f, 4.0f, water, 3, 3, 9, 9);
    const ShoreGrade grade{0.03f, 1.0f, 3.0f};

    grade_water_shore(h, n, n, water, n - 1, n - 1, grade);
    const std::vector<float> once = h;
    grade_water_shore(h, n, n, water, n - 1, n - 1, grade);

    for (std::size_t i = 0; i < h.size(); ++i) {
        expect_near(h[i], once[i], 1e-6f, "grade: a second pass changes nothing");
    }
}

/** @brief Over a borrowed grid, a terrace in steps and a level in metres are the same number. */
COOPA_TEST(a_borrowed_grid_speaks_terraces) {
    toy::world::TileMapData d;
    d.resize(8, 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) d.at(x, y).steps = (x == 4) ? 1 : 5;
    }
    TerrainField f = borrow_tiles(d);
    flood_water(f, {{4, 4}}, 3.0f, WaterKind::Lake);
    expect(d.at(4, 4).water() && d.at(4, 4).water_level() == 3,
           "borrowed: level 3.0 is terrace 3 in the record the caller still owns");
    expect(d.at(4, 4).water_depth == 2, "borrowed: ...which is two steps over ground at 1");
    expect(!d.at(3, 4).water(), "borrowed: the bank beside it stays dry");
}
