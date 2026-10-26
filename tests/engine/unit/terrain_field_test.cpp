/**
 * @file terrain_field_test.cpp
 * @brief The unified terrain field, device-free: the two modes answer the same questions over
 *        their own planes, a tile's sub-grid aliases the plane it came from, water is a surface
 *        level in both modes, and one journal covers every plane.
 *
 * The claim under test is that a caller -- a brush, the foliage scatter, GroundProbe, gameplay --
 * never needs to know which mode it is looking at. Each test therefore asks the same thing of a
 * Tiles field and a Heightmap field and holds the answers to the same contract.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <glm/glm.hpp>
#include <toyengine/world/terrain_field.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_field");

using toy::world::ConstTileHeights;
using toy::world::IRect;
using toy::world::TerrainField;
using toy::world::TerrainFieldCursor;
using toy::world::TerrainFieldGeometry;
using toy::world::TerrainFieldView;
using toy::world::TerrainMode;
using toy::world::TileHeights;
using toy::world::TileRecord;
using toy::world::WaterCell;
using toy::world::WaterKind;

namespace {

/** @brief 8 x 8 one-metre tiles of terraced ground, two steps high at x >= 4. */
TerrainField tiled() {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Tiles;
    g.tiles_x = g.tiles_y = 8;
    g.size = glm::vec2(8.0f);
    g.height_step = 0.5f;
    g.chunk_tiles = 4;
    TerrainField f;
    f.reset(g);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            TileRecord& r = f.tiles()->at(x, y);
            r.steps = x >= 4 ? 2 : 0;
            r.set_top_kind(x >= 4 ? toy::world::TileKind::Stone : toy::world::TileKind::Grass);
        }
    }
    f.mark_all_dirty();
    return f;
}

/** @brief 8 x 8 two-metre tiles with two height samples per tile edge, ground = 0.5 * local x. */
TerrainField smooth() {
    TerrainFieldGeometry g;
    g.mode = TerrainMode::Heightmap;
    g.tiles_x = g.tiles_y = 8;
    g.size = glm::vec2(16.0f);
    g.height = 20.0f;
    g.height_sub = 2;   // 17 x 17 samples over 16 m: one metre apart
    g.layer_sub = 2;
    g.chunk_tiles = 4;
    TerrainField f;
    f.reset(g);
    toy::world::HeightmapData& h = *f.heights();
    for (int j = 0; j < h.ny(); ++j) {
        for (int i = 0; i < h.nx(); ++i) h.at(i, j) = 0.5f * static_cast<float>(i);
    }
    f.mark_all_dirty();
    return f;
}

} // namespace

/** @brief The geometry derives its planes from the tile grid and the sub-resolutions. */
COOPA_TEST(sub_resolution_sizes_the_planes) {
    const TerrainField t = tiled();
    expect(t.geometry().height_sub == 1, "tiles: one height step per tile, whatever was asked for");
    expect(t.heights() == nullptr, "tiles: no height plane -- the record's step IS the height");
    expect_near(t.geometry().tile_size(), 1.0f, 1e-6f, "tiles: 8 m over 8 tiles");

    const TerrainField s = smooth();
    require(s.heights() != nullptr, "heightmap: a height plane exists");
    expect(s.heights()->nx() == 17, "heightmap: 8 tiles x 2 sub + 1 = 17 samples a side");
    expect(s.heights()->splat_w() == 16, "heightmap: 8 tiles x 2 sub = 16 layer texels a side");
    expect_near(s.geometry().cell().x, 1.0f, 1e-6f, "heightmap: 16 m over 16 sample gaps");
    expect_near(s.geometry().tile_size(), 2.0f, 1e-6f, "heightmap: two-metre tiles");
}

/** @brief Ground, normals and the dominant layer read the same way in either mode. */
COOPA_TEST(ground_and_layers_answer_in_both_modes) {
    const TerrainField t = tiled();
    expect_near(t.ground_at(glm::vec2(1.5f, 1.5f)), 0.0f, 1e-6f, "tiles: the low terrace");
    expect_near(t.ground_at(glm::vec2(5.5f, 1.5f)), 1.0f, 1e-6f, "tiles: two steps of 0.5 m");
    expect(t.dominant_layer_at(glm::vec2(5.5f, 1.5f)) == static_cast<int>(toy::world::TileKind::Stone),
           "tiles: the surface nibble IS the layer index");
    expect_near(t.layer_weight_at(static_cast<int>(toy::world::TileKind::Stone), glm::vec2(5.5f, 1.5f)), 1.0f,
                1e-6f, "tiles: a tile wears one layer at full weight");
    expect_near(t.layer_weight_at(static_cast<int>(toy::world::TileKind::Grass), glm::vec2(5.5f, 1.5f)), 0.0f,
                1e-6f, "tiles: and none of any other");

    TerrainField s = smooth();
    expect_near(s.ground_at(glm::vec2(4.0f, 2.0f)), 2.0f, 1e-5f, "heightmap: bilinear over the plane");
    expect_near(s.ground_at(glm::vec2(4.5f, 2.0f)), 2.25f, 1e-5f, "heightmap: between samples too");
    const glm::vec3 n = s.normal_at(glm::vec2(8.0f, 8.0f));
    expect(glm::length(n - glm::normalize(glm::vec3(-0.5f, 0.0f, 1.0f))) < 1e-4f,
           "heightmap: the normal follows the slope");
    expect(t.normal_at(glm::vec2(1.5f, 1.5f)).z > 0.99f, "tiles: a column's top is flat");

    s.heights()->fill_layer(3);
    expect(s.dominant_layer_at(glm::vec2(4.0f, 4.0f)) == 3, "heightmap: the painted layer dominates");
}

/** @brief A tile's height sub-grid is a view into the plane, not a copy of it. */
COOPA_TEST(tile_sub_grid_aliases_the_plane) {
    TerrainField s = smooth();
    const ConstTileHeights c = s.tile_height_samples(2, 3);
    require(static_cast<bool>(c), "heightmap: tile (2, 3) has its own samples");
    expect(c.n == 3, "heightmap: height_sub 2 means 3 samples a tile edge (its corners)");
    expect_near(c.at(0, 0), s.heights()->at(4, 6), 1e-6f, "heightmap: sample (0,0) is plane texel (4, 6)");
    expect_near(c.at(2, 0), s.heights()->at(6, 6), 1e-6f, "heightmap: and (2,0) is (6, 6)");

    TileHeights w = s.tile_height_samples(2, 3);
    w.at(1, 1) = 99.0f;
    expect_near(s.heights()->at(5, 7), 99.0f, 1e-6f, "heightmap: writing the view writes the plane");

    // Neighbouring tiles share the corner between them, which is what keeps a seam closed.
    const ConstTileHeights right = s.tile_height_samples(3, 3);
    expect_near(right.at(0, 0), c.at(2, 0), 1e-6f, "heightmap: neighbours share their edge samples");

    const TerrainField t = tiled();
    expect(!static_cast<bool>(t.tile_height_samples(2, 3)), "tiles: no plane, so no sub-grid view");
}

/** @brief A tile's layer weights read through one call in either mode. */
COOPA_TEST(tile_layer_weights_read_in_both_modes) {
    const TerrainField t = tiled();
    const auto tl = t.tile_layer_weights(5, 1);
    require(static_cast<bool>(tl), "tiles: tile (5, 1) has layer weights");
    expect(tl.n == 1, "tiles: one layer texel per tile");
    expect(tl.weight(0, 0, static_cast<int>(toy::world::TileKind::Stone)) == 255, "tiles: its layer, full weight");
    expect(tl.dominant(0, 0) == static_cast<int>(toy::world::TileKind::Stone), "tiles: and it dominates");

    TerrainField s = smooth();
    s.heights()->fill_layer(1);
    const auto sl = s.tile_layer_weights(5, 1);
    require(static_cast<bool>(sl), "heightmap: tile (5, 1) has layer weights");
    expect(sl.n == 2, "heightmap: layer_sub 2 means 2 x 2 texels per tile");
    expect(sl.weight(1, 1, 1) == 255, "heightmap: the filled layer carries the weight");
    expect(sl.dominant(0, 1) == 1, "heightmap: and dominates every texel of the tile");
}

/** @brief Water is a kind plus a surface level, and never stands at or under the ground. */
COOPA_TEST(water_is_a_surface_level_in_both_modes) {
    TerrainField t = tiled();
    t.set_water(glm::ivec2(1, 1), WaterCell{WaterKind::Ocean, 1.5f, 0});
    const WaterCell tw = t.water_at(glm::ivec2(1, 1));
    expect(tw.kind == WaterKind::Ocean, "tiles: the kind round-trips");
    expect_near(tw.level, 1.5f, 1e-6f, "tiles: three 0.5 m steps of water over flat ground");
    expect(t.tiles()->at(1, 1).water_depth == 3, "tiles: the record holds it as steps, as the file does");

    t.set_water(glm::ivec2(2, 2), WaterCell{WaterKind::River, 1.0f, 5});
    const WaterCell river = t.water_at(glm::ivec2(2, 2));
    expect(river.kind == WaterKind::River && river.flow == 5, "tiles: a river carries its flow octant");

    // The ground of tile (5, 1) is 1.0 m; water asked for at 0.5 m cannot stand there.
    t.set_water(glm::ivec2(5, 1), WaterCell{WaterKind::Lake, 0.5f, 0});
    expect(!t.water_at(glm::ivec2(5, 1)).wet(), "tiles: a level under the ground drains the tile");

    TerrainField s = smooth();
    // Tile (1, 1)'s centre is local (3, 3) m, so its ground is 1.5 m.
    s.set_water(glm::ivec2(1, 1), WaterCell{WaterKind::Lake, 4.25f, 0});
    const WaterCell sw = s.water_at(glm::ivec2(1, 1));
    expect(sw.kind == WaterKind::Lake, "heightmap: the kind round-trips");
    expect_near(sw.level, 4.25f, 1e-6f, "heightmap: the level is exact, not quantised to a step");
    s.set_water(glm::ivec2(1, 1), WaterCell{WaterKind::Lake, 0.25f, 0});
    expect(!s.water_at(glm::ivec2(1, 1)).wet(), "heightmap: a level under the ground drains the tile too");
}

/** @brief Texel rectangles in either plane map onto the tiles they cover. */
COOPA_TEST(texels_map_onto_tiles) {
    const TerrainField s = smooth();   // height_sub 2, layer_sub 2, 8 x 8 tiles

    // Layer texels are cell-centred inside a tile: two per tile edge, so 4 texels are 2 tiles.
    expect(s.tiles_of_layer_texels(IRect{0, 0, 4, 4}) == IRect{0, 0, 2, 2}, "layers: 4 texels are 2 tiles");
    expect(s.tiles_of_layer_texels(IRect{5, 5, 6, 6}) == IRect{2, 2, 3, 3}, "layers: texel 5 is tile 2");
    expect(s.tiles_of_layer_texels(IRect{}).empty(), "layers: an empty rect maps to nothing");
    expect(s.tiles_of_layer_texels(IRect{0, 0, 999, 999}) == IRect{0, 0, 8, 8},
           "layers: a rect past the plane is clipped to the grid");

    // Height samples sit on tile CORNERS, so a texel belongs to the tiles on both sides of it.
    const IRect h = s.tiles_of_height_texels(IRect{8, 8, 9, 9});
    expect(h.x0 <= 3 && h.x1 >= 5, "heights: texel 8 is tile 4's corner, so tiles 3..4 are covered");
    expect(s.tiles_of_height_texels(IRect{0, 0, 1, 1}) == IRect{0, 0, 2, 2},
           "heights: the first texel is clipped at the grid's edge, not negative");
    expect(s.tiles_of_height_texels(IRect{}).empty(), "heights: an empty rect maps to nothing");

    const TerrainField t = tiled();    // height_sub 1
    expect(t.tiles_of_layer_texels(IRect{3, 4, 4, 5}) == IRect{3, 4, 4, 5},
           "tiles: one layer texel per tile, so the mapping is the identity");
}

/** @brief One journal over every plane, read through a per-plane cursor. */
COOPA_TEST(one_journal_covers_every_plane) {
    TerrainField s = smooth();
    const TerrainFieldCursor base = s.cursor();

    s.mark_tiles_dirty(IRect{2, 3, 4, 5});
    const IRect r = s.dirty_since(base);
    expect(!r.empty(), "heightmap: the edit is reported");
    expect(r.x0 <= 2 && r.y0 <= 3 && r.x1 >= 4 && r.y1 >= 5,
           "heightmap: and covers at least the tiles that were marked");
    expect(r.x1 <= s.geometry().tiles_x && r.y1 <= s.geometry().tiles_y,
           "heightmap: never past the grid, whatever the plane's texel counts are");
    expect(s.cursor() != base, "heightmap: the cursor moved");
    expect(s.dirty_since(s.cursor()).empty(), "heightmap: nothing is pending at the new cursor");

    // A splat-only edit still comes back in TILE space, so one consumer re-reads one region.
    const TerrainFieldCursor mid = s.cursor();
    s.heights()->mark_splat_dirty(IRect{0, 0, 4, 4});
    const IRect sr = s.dirty_since(mid);
    expect(sr.x0 == 0 && sr.y0 == 0 && sr.x1 >= 2 && sr.y1 >= 2,
           "heightmap: a splat edit covers the tiles it painted");

    // Two consumers at different cursors each see their own outstanding work. The rects are a
    // conservative cover (the journals coalesce), so the older cursor can only see more.
    TerrainField t = tiled();
    const TerrainFieldCursor a = t.cursor();
    t.mark_tiles_dirty(IRect{1, 1, 2, 2});
    const TerrainFieldCursor b = t.cursor();
    t.mark_tiles_dirty(IRect{5, 5, 6, 6});
    const IRect from_a = t.dirty_since(a), from_b = t.dirty_since(b);
    expect(from_b == IRect{5, 5, 6, 6}, "tiles: the newer cursor sees the second edit");
    expect(from_a.x0 <= 1 && from_a.y0 <= 1 && from_a.x1 >= 6 && from_a.y1 >= 6,
           "tiles: the older cursor sees both");
    expect(t.dirty_since(t.cursor()).empty(), "tiles: and nothing once it has caught up");
}

/** @brief The world-space view adds the owner's origin to every answer, water included. */
COOPA_TEST(view_offsets_into_the_world) {
    const TerrainField t = tiled();
    TerrainField s = smooth();
    s.set_water(glm::ivec2(1, 1), WaterCell{WaterKind::Lake, 4.0f, 0});

    const TerrainFieldView tv{&t, glm::vec3(-4.0f, -4.0f, 10.0f)};
    expect(tv.contains(glm::vec3(0.0f, 0.0f, 0.0f)), "view: the middle of the terrain is on it");
    expect(!tv.contains(glm::vec3(100.0f, 0.0f, 0.0f)), "view: a point past its edge is not");
    expect_near(tv.height(glm::vec3(1.5f, -2.5f, 0.0f)), 11.0f, 1e-5f, "view: ground + the origin's z");

    const TerrainFieldView sv{&s, glm::vec3(0.0f, 0.0f, 5.0f)};
    const WaterCell w = sv.water(glm::vec3(3.0f, 3.0f, 0.0f));
    expect(w.kind == WaterKind::Lake, "view: the water kind comes through");
    expect_near(w.level, 9.0f, 1e-5f, "view: and its level is in world space");
}
