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
using toy::world::TerraData;
using toy::world::TerraGeometry;
using toy::world::TerrainField;
using toy::world::TileCell;
using toy::world::TileKind;
using toy::world::WaterKind;

namespace {

std::shared_ptr<TerraData> flat(int w, int h, int steps) {
    TerraGeometry g;
    g.tiles_x = w;
    g.tiles_y = h;
    auto d = std::make_shared<TerraData>(g);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) d->at(x, y).steps = static_cast<std::int16_t>(steps);
    d->mark_all_dirty();
    return d;
}

/** @brief A field over `d`, sharing the same live data -- edits through either are visible in both. */
TerrainField field_of(const std::shared_ptr<TerraData>& d) {
    TerrainField f;
    f.attach(d);
    return f;
}

int count_steps(const TerraData& d, int steps) {
    int n = 0;
    for (const TileCell& r : d.tiles()) n += r.steps == steps;
    return n;
}

/** @brief Depth, in whole steps, of a wet cell's water below its own bed. */
int depth_steps(const TileCell& c, float height_step) {
    return static_cast<int>(c.water_level / height_step) - c.steps;
}

} // namespace

COOPA_TEST(brush_rect_is_a_centred_square) {
    expect(tile_brush_rect({5, 5}, 1) == (IRect{5, 5, 6, 6}), "size 1 is the tile itself");
    expect(tile_brush_rect({5, 5}, 3) == (IRect{4, 4, 7, 7}), "odd sizes centre on the tile");
    expect(tile_brush_rect({5, 5}, 2) == (IRect{5, 5, 7, 7}), "even sizes lean toward +x / +y");
}

COOPA_TEST(raise_and_lower_move_whole_steps_inside_the_brush_only) {
    auto d = flat(10, 10, 3);
    TerrainField f = field_of(d);
    const std::uint64_t rev = d->cursor().tiles;
    const IRect changed = tile_raise(f, tile_brush_rect({5, 5}, 3), 2);
    expect(changed == (IRect{4, 4, 7, 7}) && d->cursor().tiles > rev, "raise reports and marks what it changed");
    expect(count_steps(*d, 5) == 9 && count_steps(*d, 3) == 91, "exactly the 3 x 3 square rose by 2");
    tile_raise(f, IRect{0, 0, 10, 10}, -10);
    expect(count_steps(*d, 0) == 100, "lowering never goes below the ground plane");
    expect(tile_raise(f, IRect{20, 20, 25, 25}, 1).empty(), "a brush off the grid changes nothing");
}

COOPA_TEST(raise_on_water_moves_its_bed) {
    auto d = flat(4, 4, 7);   // banks at 7
    TerrainField f = field_of(d);
    TileCell& lake = d->at(1, 1);
    lake.steps = 3;
    lake.water_kind = static_cast<std::uint8_t>(WaterKind::Lake);
    lake.water_level = 6.0f;   // bed 3, water to 6
    tile_raise(f, IRect{1, 1, 2, 2}, 1);
    expect(lake.steps == 4 && lake.water_level == 6.0f && depth_steps(lake, 1.0f) == 2,
           "raising a lake raises its bed, not its surface");
    tile_raise(f, IRect{1, 1, 2, 2}, 5);
    expect(!lake.wet() && lake.steps == 9, "raising the bed past the surface leaves land");
}

COOPA_TEST(lowering_beside_water_floods) {
    auto d = flat(5, 1, 4);
    TerrainField f = field_of(d);
    d->at(0, 0).steps = 1;
    d->at(0, 0).water_kind = static_cast<std::uint8_t>(WaterKind::Lake);
    d->at(0, 0).water_level = 3.0f;
    const IRect changed = tile_raise(f, IRect{1, 0, 2, 1}, -3);   // dig the bank beside it down to 1
    expect(d->at(1, 0).wet() && d->at(1, 0).water_level == 3.0f, "the dug tile floods from the water beside it");
    expect(changed.x0 == 1 && changed.x1 == 2, "...as part of the same edit");
    expect(!d->at(2, 0).wet(), "and no further than the ground holds it");
}

COOPA_TEST(set_height_paint_and_smooth) {
    auto d = flat(8, 8, 2);
    TerrainField f = field_of(d);
    tile_set_height(f, IRect{0, 0, 2, 2}, 7);
    expect(count_steps(*d, 7) == 4, "set height levels the brush to the target");

    d->at(5, 5).water_kind = static_cast<std::uint8_t>(WaterKind::Lake);
    d->at(5, 5).water_level = 4.0f;
    tile_paint(f, IRect{4, 4, 7, 7}, TileKind::Sand, true);
    expect(d->at(4, 4).top_kind() == TileKind::Sand && d->at(4, 4).side_kind() == TileKind::Sand,
           "paint sets the top kind and its usual side");
    expect(d->at(5, 5).top_kind() == TileKind::Sand && d->at(5, 5).wet(), "a bed under water paints like any ground");

    auto spike = flat(5, 5, 2);
    TerrainField sf = field_of(spike);
    spike->at(2, 2).steps = 9;
    tile_smooth(sf, IRect{0, 0, 5, 5});
    expect(spike->at(2, 2).steps == 8, "smooth moves a spike one step toward its neighbourhood");
    expect(spike->at(1, 1).steps == 2, "...and leaves its level neighbours be");
}

COOPA_TEST(water_floods_its_basin_and_drains_the_body) {
    auto d = flat(6, 6, 2);
    TerrainField f = field_of(d);
    d->at(0, 0).steps = 8;   // high ground stays dry
    for (int y = 0; y < 6; ++y) d->at(4, y).steps = 9;   // a ridge: the basin ends at x = 4
    const IRect r = tile_water(f, IRect{0, 0, 2, 2}, 5, true);   // the brush takes in the high tile
    const TileCell& w = d->at(1, 1);
    expect(w.wet() && w.steps == 2 && w.water_level == 5.0f && depth_steps(w, 1.0f) == 3,
           "flooding fills over the ground, which stays the bed");
    expect(d->at(3, 5).wet() && !d->at(5, 5).wet(), "the flood runs through the basin, not over the ridge");
    expect(r.x1 == 4 && r.y1 == 6, "...and reports the whole basin it changed");
    expect(!d->at(0, 0).wet(), "ground above the level stays dry");
    tile_water(f, IRect{2, 2, 3, 3}, 0, false);
    expect(!d->at(1, 1).wet() && d->at(1, 1).steps == 2 && !d->at(3, 5).wet(), "draining takes the whole body back to its bed");
}

COOPA_TEST(selections_rectangles_similar_and_edits) {
    auto d = flat(10, 10, 1);
    TerrainField f = field_of(d);
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
    tile_select_similar(f, {1, 1}, false, sel);
    expect(sel.count() == 16 && !sel.contains(8, 8), "similar selects the connected tiles of the same height only");

    const IRect changed = tile_edit_selection(f, sel, [](TileCell& r) { r.set_top_kind(TileKind::Snow); r.biome = 4; });
    expect(changed == (IRect{0, 0, 4, 4}), "editing a selection reports its bounds");
    expect(d->at(3, 3).top_kind() == TileKind::Snow && d->at(5, 5).top_kind() == TileKind::Grass,
           "...and changes the selected tiles only");
}

COOPA_TEST(undo_patch_restores_exact_tiles) {
    auto d = flat(12, 12, 4);
    TerrainField f = field_of(d);
    const auto before_all = d->tiles();
    const IRect r = tile_brush_rect({6, 6}, 5);
    TilePatch before = capture_tile_patch(d, r);
    tile_raise(f, r, 3);
    tile_paint(f, r, TileKind::Ash, true);
    expect(d->tiles() != before_all, "the edits changed the tiles");
    expect(apply_tile_patch(before) && d->tiles() == before_all, "the patch restores the exact tiles");

    auto other = flat(12, 12, 4);
    TilePatch stale = before;
    stale.data = other;
    expect(!apply_tile_patch(stale), "a patch never lands on another terrain's tiles");
}

} // namespace toy::editor::testing
