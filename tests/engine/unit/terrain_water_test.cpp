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
