/**
 * @file tile_brush_test.cpp
 * @brief The TileTerrain tools on a known grid: square brush rects, Raise / Lower in whole steps
 *        (never below 0, a lake's bed rather than its surface), Set Height, Paint (water is left
 *        alone), Smooth's one-step pull toward the neighbourhood, Water flood and drain back to the
 *        bed, rectangle and "similar" selections, selection edits, and an undo patch restoring the
 *        exact tiles.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include "editor/terrain/tile_brush.h"

COOPA_TEST_SUITE("tile_brush");

namespace toy::editor::testing {

using coopa::test::expect;
using toy::world::IRect;
using toy::world::TileKind;
using toy::world::TileMapData;
using toy::world::TileRecord;

namespace {

std::shared_ptr<TileMapData> flat(int w, int h, int steps) {
    auto d = std::make_shared<TileMapData>();
    d->resize(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) d->at(x, y).steps = static_cast<std::int16_t>(steps);
    return d;
}

int count_steps(const TileMapData& d, int steps) {
    int n = 0;
    for (const TileRecord& r : d.records()) n += r.steps == steps;
    return n;
}

} // namespace

COOPA_TEST(brush_rect_is_a_centred_square) {
    expect(tile_brush_rect({5, 5}, 1) == (IRect{5, 5, 6, 6}), "size 1 is the tile itself");
    expect(tile_brush_rect({5, 5}, 3) == (IRect{4, 4, 7, 7}), "odd sizes centre on the tile");
    expect(tile_brush_rect({5, 5}, 2) == (IRect{5, 5, 7, 7}), "even sizes lean toward +x / +y");
}

COOPA_TEST(raise_and_lower_move_whole_steps_inside_the_brush_only) {
    auto d = flat(10, 10, 3);
    const std::uint64_t rev = d->revision();
    const IRect changed = tile_raise(*d, tile_brush_rect({5, 5}, 3), 2);
    expect(changed == (IRect{4, 4, 7, 7}) && d->revision() > rev, "raise reports and marks what it changed");
    expect(count_steps(*d, 5) == 9 && count_steps(*d, 3) == 91, "exactly the 3 x 3 square rose by 2");
    tile_raise(*d, IRect{0, 0, 10, 10}, -10);
    expect(count_steps(*d, 0) == 100, "lowering never goes below the ground plane");
    expect(tile_raise(*d, IRect{20, 20, 25, 25}, 1).empty(), "a brush off the grid changes nothing");
}

COOPA_TEST(raise_on_water_moves_its_bed) {
    auto d = flat(4, 4, 2);
    TileRecord& lake = d->at(1, 1);
    lake.steps = 6;
    lake.water_depth = 3;
    lake.set(TileRecord::Water, true);
    tile_raise(*d, IRect{1, 1, 2, 2}, 1);
    expect(lake.steps == 6 && lake.water_depth == 2 && lake.water(), "raising a lake raises its bed, not its surface");
    tile_raise(*d, IRect{1, 1, 2, 2}, 5);
    expect(!lake.water() && lake.steps == 9, "raising the bed past the surface leaves land");
}

COOPA_TEST(set_height_paint_and_smooth) {
    auto d = flat(8, 8, 2);
    tile_set_height(*d, IRect{0, 0, 2, 2}, 7);
    expect(count_steps(*d, 7) == 4, "set height levels the brush to the target");

    d->at(5, 5).set(TileRecord::Water, true);
    tile_paint(*d, IRect{4, 4, 7, 7}, TileKind::Sand, true);
    expect(d->at(4, 4).top_kind() == TileKind::Sand && d->at(4, 4).side_kind() == TileKind::Sand,
           "paint sets the top kind and its usual side");
    expect(d->at(5, 5).top_kind() != TileKind::Sand, "paint leaves water alone");

    auto spike = flat(5, 5, 2);
    spike->at(2, 2).steps = 9;
    tile_smooth(*spike, IRect{0, 0, 5, 5});
    expect(spike->at(2, 2).steps == 8, "smooth moves a spike one step toward its neighbourhood");
    expect(spike->at(1, 1).steps == 2, "...and leaves its level neighbours be");
}

COOPA_TEST(water_floods_to_a_level_and_drains_to_the_bed) {
    auto d = flat(6, 6, 2);
    d->at(0, 0).steps = 8;   // high ground stays dry
    tile_water(*d, IRect{0, 0, 3, 3}, 5, true);
    const TileRecord& w = d->at(1, 1);
    expect(w.water() && w.steps == 5 && w.water_depth == 3 && w.top_kind() == TileKind::Water,
           "flooding raises the surface to the level and remembers the depth");
    expect(!d->at(0, 0).water(), "ground above the level stays dry");
    tile_water(*d, IRect{0, 0, 3, 3}, 0, false);
    expect(!d->at(1, 1).water() && d->at(1, 1).steps == 2, "draining drops back to the bed");
}

COOPA_TEST(selections_rectangles_similar_and_edits) {
    auto d = flat(10, 10, 1);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) d->at(x, y).steps = 3;   // a plateau in the corner
    d->at(8, 8).steps = 3;                                    // same height, not connected

    TileSelection sel;
    sel.reset(10, 10);
    sel.set_rect(IRect{2, 2, 5, 5}, true);
    expect(sel.count() == 9 && sel.bounds() == (IRect{2, 2, 5, 5}), "a rectangle selects its tiles");
    sel.set_rect(IRect{2, 2, 3, 3}, false);
    expect(sel.count() == 8, "removing takes tiles back out");

    sel.clear();
    tile_select_similar(*d, {1, 1}, false, sel);
    expect(sel.count() == 16 && !sel.contains(8, 8), "similar selects the connected tiles of the same height only");

    const IRect changed = tile_edit_selection(*d, sel, [](TileRecord& r) { r.set_top_kind(TileKind::Snow); r.biome = 4; });
    expect(changed == (IRect{0, 0, 4, 4}), "editing a selection reports its bounds");
    expect(d->at(3, 3).top_kind() == TileKind::Snow && d->at(5, 5).top_kind() == TileKind::Grass,
           "...and changes the selected tiles only");
}

COOPA_TEST(undo_patch_restores_exact_tiles) {
    auto d = flat(12, 12, 4);
    const auto before_all = d->records();
    const IRect r = tile_brush_rect({6, 6}, 5);
    TilePatch before = capture_tile_patch(d, r);
    tile_raise(*d, r, 3);
    tile_paint(*d, r, TileKind::Ash, true);
    expect(d->records() != before_all, "the edits changed the tiles");
    expect(apply_tile_patch(before) && d->records() == before_all, "the patch restores the exact tiles");

    auto other = flat(12, 12, 4);
    TilePatch stale = before;
    stale.data = other;
    expect(!apply_tile_patch(stale), "a patch never lands on another terrain's tiles");
}

} // namespace toy::editor::testing
