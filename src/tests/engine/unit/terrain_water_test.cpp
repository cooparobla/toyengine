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

using toy::world::borrow;
using toy::world::IRect;
using toy::world::k_water_flood_cap;
using toy::world::ShoreGrade;
using toy::world::TerraData;
using toy::world::TerraGeometry;
using toy::world::TerrainField;
using toy::world::TileCell;
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
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 9;
    g.tile_size = 1.0f;
    g.height_step = 1.0f;
    TerrainField f;
    f.reset(g);
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 9; ++x) {
            f.data()->at(x, y).steps = (x >= 3 && x <= 5 && y >= 3 && y <= 5) ? 2 : 5;
        }
    }
    f.data()->at(6, 4).steps = 3;
    f.mark_all_dirty();
    return f;
}

/**
 * @brief 8 x 8 two-metre tiles over a 17 x 17 sample plane (height_sub 2), ground 5 m with a
 *        smooth bowl of 2 m.
 *
 * The corners in [6, 12] are 2 m, so the nine tiles (3, 4, 5 each way) whose four corners all
 * lie inside that block read exactly 2 m and everything around them reads higher -- a bowl with
 * a sloped rim rather than a terrace with a wall.
 */
TerrainField smooth_basin() {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 8;
    g.tile_size = 2.0f;
    g.height = 20.0f;
    g.height_sub = 2;
    g.layer_sub = 1;
    TerrainField f;
    f.reset(g);
    TerraData* d = f.data();
    for (int j = 0; j < d->height_ny(); ++j) {
        for (int i = 0; i < d->height_nx(); ++i) {
            const bool in_bowl = i >= 6 && i <= 12 && j >= 6 && j <= 12;
            d->height_at(i, j) = in_bowl ? 2.0f : 5.0f;
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
    t.data()->at(4, 4).steps = 1;
    t.mark_tiles_dirty(IRect{4, 4, 5, 5});
    settle_water(t, IRect{4, 4, 5, 5});

    const WaterCell filled = t.water_at(glm::ivec2(4, 4));
    expect(filled.kind == WaterKind::River, "tiles: the dug tile fills as river water");
    expect(filled.flow == 3, "tiles: ...flowing the same way");
    expect_near(filled.level, 4.0f, 1e-5f, "tiles: ...at the river's level");

    // Ocean and Lake are different kinds, and a spread keeps them apart.
    TerrainField o = tiled_basin();
    o.set_water(glm::ivec2(3, 3), WaterCell{WaterKind::Ocean, 4.0f, 0});
    o.data()->at(4, 3).steps = 1;
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
 * Water's level is an absolute-metres field on the cell now, independent of `steps`, in every
 * field -- so raising a tile's ground past its own water's level does not touch the water at
 * all; the cell is left wet with ground above its own surface until settle_water() removes it.
 * One behaviour, not a per-mode split (the old Tiles-only `steps + water_depth` encoding this
 * used to need is gone with `water_depth` itself).
 */
COOPA_TEST(ground_raised_through_the_surface_dries) {
    TerrainField t = tiled_basin();
    flood_water(t, {{4, 4}}, 4.0f, WaterKind::Lake);
    t.data()->at(4, 4).steps = 6;   // raises the bed through the water; water_level is untouched
    t.mark_tiles_dirty(IRect{4, 4, 5, 5});
    expect(t.water_at(glm::ivec2(4, 4)).wet(),
           "tiles: the ground is above the level and the water is still recorded");
    settle_water(t, IRect{4, 4, 5, 5});
    expect(!t.water_at(glm::ivec2(4, 4)).wet(), "tiles: ...and the settle is what removes it");
    expect(t.water_at(glm::ivec2(3, 4)).wet() && t.water_at(glm::ivec2(5, 4)).wet(),
           "tiles: ...and the water around it stays");

    TerrainField s = smooth_basin();
    flood_water(s, {{4, 4}}, 2.5f, WaterKind::Lake);
    TerraData* d = s.data();
    for (int j = 8; j <= 10; ++j) {
        for (int i = 8; i <= 10; ++i) d->height_at(i, j) = 6.0f;
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
 */
COOPA_TEST(a_budgeted_settle_matches_one_call_on_a_smooth_field) {
    // A long trough one tile wide, with water at one end: a flood with a lot of tiles to cross.
    const auto build = [] {
        TerraGeometry g;
        g.tiles_x = g.tiles_y = 32;
        g.tile_size = 1.0f;
        g.height = 20.0f;
        g.height_sub = 2;
        TerrainField f;
        f.reset(g);
        TerraData* h = f.data();
        for (int j = 0; j < h->height_ny(); ++j) {
            for (int i = 0; i < h->height_nx(); ++i) h->height_at(i, j) = (j >= 28 && j <= 36) ? 1.0f : 6.0f;
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

/** @brief Over a borrowed grid, water lands directly in the caller's own TerraData, exactly. */
COOPA_TEST(a_borrowed_grid_speaks_in_exact_metres) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 8;
    g.tile_size = 1.0f;
    g.height_step = 1.0f;
    TerraData d(g);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) d.at(x, y).steps = (x == 4) ? 1 : 5;
    }
    d.mark_all_dirty();
    TerrainField f = borrow(d);
    flood_water(f, {{4, 4}}, 3.0f, WaterKind::Lake);
    expect(d.at(4, 4).wet() && d.at(4, 4).water_kind == static_cast<std::uint8_t>(WaterKind::Lake),
           "borrowed: the water lands in the record the caller still owns");
    expect_near(d.at(4, 4).water_level, 3.0f, 1e-6f,
               "borrowed: the level is kept exact, in metres -- not quantised to a terrace");
    expect(!d.at(3, 4).wet(), "borrowed: the bank beside it stays dry");
}

/**
 * @brief A ridge standing inside the channel is sunk under the water; the same ridge on a sample
 *        a dry tile shares is left alone, because that one is a bank.
 *
 * Pass 1 only guarantees a wet tile's CENTRE -- the bilinear value `set_water()`'s dryness test
 * reads -- and a centre is a mean, so three corners on the bed hide a fourth standing proud of
 * the water. The surface sits exactly at the level, so that fourth sample is terrain poking
 * through it. Pass 2 is what closes that, and it has to close it without touching the shoreline:
 * a sample any dry tile shares still describes dry ground, and lowering it would cut a notch in
 * the bank instead.
 */
COOPA_TEST(grade_water_shore_sinks_a_ridge_inside_the_channel) {
    constexpr int n = 9;   // 8 x 8 tiles
    std::vector<WaterCell> water;
    std::vector<float> h = flat_plane_with_lake(n, 2.0f, 4.0f, water, 2, 2, 6, 6);
    h[4 * static_cast<std::size_t>(n) + 4] = 5.0f;   // every tile touching it is wet: the interior
    h[2 * static_cast<std::size_t>(n) + 2] = 5.0f;   // only one of its four tiles is wet: a bank

    // Both ridges are invisible to pass 1: each of their tiles keeps three corners on the 2 m bed,
    // so every centre reads 2.75 -- a metre clear of the 4 m level -- and reports nothing to fix.
    for (int y = 2; y < 6; ++y) {
        for (int x = 2; x < 6; ++x) {
            require(tile_mean(h, n, x, y) <= 4.0f - 0.03f,
                    "interior: pass 1 sees nothing wrong with this bed, so pass 2 is what acts");
        }
    }

    grade_water_shore(h, n, n, water, n - 1, n - 1, ShoreGrade{0.03f, 0.0f, 4.0f});

    expect(h[4 * static_cast<std::size_t>(n) + 4] <= 4.0f - 0.03f + 1e-5f,
           "interior: a ridge whose every tile is wet is sunk under the water standing over it");
    expect_near(h[2 * static_cast<std::size_t>(n) + 2], 5.0f, 1e-6f,
                "interior: the same ridge where a dry tile shares the sample is left alone -- a bank");
}

/**
 * @brief Pass 2 holds the interior under the LOWEST level over it, where two bodies meet.
 *
 * The same rule the mesh builder's corners follow (terrain_water_mesh.cpp's corner_at): a sample
 * under two bodies at different levels has to clear the lower one, or the lower body's own
 * surface is the one the bed comes through.
 */
COOPA_TEST(grade_water_shore_interior_clears_the_lower_of_two_levels) {
    constexpr int n = 9;
    std::vector<WaterCell> water;
    std::vector<float> h = flat_plane_with_lake(n, 2.0f, 6.0f, water, 2, 2, 6, 6);
    // The high-x half of the lake is a second, lower body at 4 m: the sample column x = 4 is
    // interior to both.
    for (int y = 2; y < 6; ++y) {
        for (int x = 4; x < 6; ++x) water[static_cast<std::size_t>(y) * (n - 1) + x] = WaterCell{WaterKind::Lake, 4.0f, 0};
    }
    h[4 * static_cast<std::size_t>(n) + 4] = 5.0f;   // under the 6 m body, over the 4 m one

    grade_water_shore(h, n, n, water, n - 1, n - 1, ShoreGrade{0.03f, 0.0f, 4.0f});

    expect(h[4 * static_cast<std::size_t>(n) + 4] <= 4.0f - 0.03f + 1e-5f,
           "interior: a sample under two bodies clears the lower level, not the higher one");
}
