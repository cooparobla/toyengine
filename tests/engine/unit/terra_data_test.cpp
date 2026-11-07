/**
 * @file terra_data_test.cpp
 * @brief `TerraData` (toyengine/world/terra_data.h): the one store a terrain's shape, layers,
 *        water, biome and climate live in, whichever meshing style draws it. Replaces
 *        tile_map_test.cpp's and heightmap_data_test.cpp's coverage of the two stores it unifies.
 *
 * Every test asks the same question with and without a height plane (`height_sub == 1` vs
 * `> 1`), because the claim under test is that `ground_at`/`normal_at`/`layer_weight` never show
 * the difference to a caller -- the same claim terrain_field_test.cpp makes of the facade this
 * type replaces the inside of.
 */

#include <coopa/testing/test.h>

#include <cmath>

#include <glm/glm.hpp>
#include <toyengine/world/terra_data.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terra_data");

using toy::world::IRect;
using toy::world::TerraCursor;
using toy::world::TerraData;
using toy::world::TerraGeometry;
using toy::world::TileCell;
using toy::world::TileKind;
using toy::world::WaterKind;

namespace {

/** @brief 8 x 8 one-metre tiles, no height plane: two terraces, stepped at x >= 4. */
TerraData terraced() {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 8;
    g.tile_size = 1.0f;
    g.height_step = 0.5f;
    g.chunk_tiles = 4;
    TerraData d(g);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            TileCell& c = d.at(x, y);
            c.steps = x >= 4 ? 2 : 0;
            c.set_top_kind(x >= 4 ? TileKind::Stone : TileKind::Grass);
        }
    }
    d.mark_all_dirty();
    return d;
}

/** @brief 8 x 8 two-metre tiles, height_sub 2: ground = 0.5 * local x, continuous. */
TerraData smooth() {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 8;
    g.tile_size = 2.0f;
    g.height_sub = 2; // 17 x 17 samples over 16 m: one metre apart
    g.layer_sub = 2;
    g.height = 20.0f;
    g.chunk_tiles = 4;
    TerraData d(g);
    for (int j = 0; j < d.height_ny(); ++j)
        for (int i = 0; i < d.height_nx(); ++i) d.height_at(i, j) = 0.5f * static_cast<float>(i);
    d.mark_all_dirty();
    return d;
}

} // namespace

/** @brief A rectangular grid (tiles_x != tiles_y) -- impossible for the facade this replaces. */
COOPA_TEST(geometry_allows_a_rectangular_grid) {
    TerraGeometry g;
    g.tiles_x = 20;
    g.tiles_y = 6;
    g.tile_size = 1.5f;
    g.height_sub = 4;
    TerraData d(g);
    expect(d.geometry().tiles_x == 20 && d.geometry().tiles_y == 6, "the grid stays rectangular");
    expect(d.height_nx() == 20 * 4 + 1 && d.height_ny() == 6 * 4 + 1, "the height plane follows each axis independently");
    expect_near(d.geometry().size().x, 30.0f, 1e-6f, "size.x is tiles_x * tile_size");
    expect_near(d.geometry().size().y, 9.0f, 1e-6f, "size.y is tiles_y * tile_size");
}

COOPA_TEST(sub_resolution_sizes_the_planes) {
    const TerraData t = terraced();
    expect(!t.has_heights(), "terraced: no height plane -- the tile's own steps IS the height");
    expect_near(t.geometry().tile_size, 1.0f, 1e-6f, "terraced: 8 m over 8 tiles");

    const TerraData s = smooth();
    expect(s.has_heights(), "smooth: a height plane exists");
    expect(s.height_nx() == 17, "smooth: 8 tiles x 2 sub + 1 = 17 samples a side");
    expect(s.layer_nx() == 16, "smooth: 8 tiles x 2 sub = 16 layer texels a side");
    expect_near(s.cell(), 1.0f, 1e-6f, "smooth: 16 m over 16 sample gaps");
}

/** @brief Ground, normals and the dominant layer read the same way whichever plane backs them. */
COOPA_TEST(ground_and_layers_answer_with_or_without_a_height_plane) {
    const TerraData t = terraced();
    expect_near(t.ground_at(glm::vec2(1.5f, 1.5f)), 0.0f, 1e-6f, "terraced: the low terrace");
    expect_near(t.ground_at(glm::vec2(5.5f, 1.5f)), 1.0f, 1e-6f, "terraced: two steps of 0.5 m");
    expect(t.normal_at(glm::vec2(1.5f, 1.5f)).z > 0.99f, "terraced: a column's top is flat");
    expect_near(t.layer_weight(static_cast<int>(TileKind::Stone), glm::vec2(5.5f, 1.5f)), 1.0f, 1e-6f,
               "terraced: a tile wears one layer at full weight");
    expect_near(t.layer_weight(static_cast<int>(TileKind::Grass), glm::vec2(5.5f, 1.5f)), 0.0f, 1e-6f,
               "terraced: and none of any other");

    TerraData s = smooth();
    expect_near(s.ground_at(glm::vec2(4.0f, 2.0f)), 2.0f, 1e-5f, "smooth: bilinear over the plane");
    expect_near(s.ground_at(glm::vec2(4.5f, 2.0f)), 2.25f, 1e-5f, "smooth: between samples too");
    const glm::vec3 n = s.normal_at(glm::vec2(8.0f, 8.0f));
    expect(glm::length(n - glm::normalize(glm::vec3(-0.5f, 0.0f, 1.0f))) < 1e-4f, "smooth: the normal follows the slope");

    s.fill_layer(3);
    const auto w = s.layer_weights(glm::vec2(4.0f, 4.0f));
    expect(w[3] > 0.99f, "smooth: the painted layer dominates");
}

/** @brief Water lives in the tile plane itself now -- kind, level, flow and the authored flag. */
COOPA_TEST(water_lives_in_the_tile_cell) {
    TerraData t = terraced();
    TileCell& c = t.at(1, 1);
    c.water_kind = static_cast<std::uint8_t>(WaterKind::Ocean);
    c.water_level = 1.5f;
    t.mark_tiles_dirty(IRect{1, 1, 2, 2});
    expect(t.at(1, 1).wet(), "a cell with a water_kind is wet()");
    expect_near(t.at(1, 1).water_level, 1.5f, 1e-6f, "the level is exact, not quantised to a step");

    TileCell& r = t.at(2, 2);
    r.water_kind = static_cast<std::uint8_t>(WaterKind::River);
    r.flow = 5;
    r.flow_authored = 1;
    t.mark_tiles_dirty(IRect{2, 2, 3, 3});
    expect(static_cast<WaterKind>(t.at(2, 2).water_kind) == WaterKind::River && t.at(2, 2).flow == 5,
           "a river carries its flow octant");
    expect(t.at(2, 2).flow_authored == 1, "the authored flag round-trips alongside it");

    expect(sizeof(TileCell) == 24, "the on-disk record stays 24 bytes");
}

/** @brief One journal per plane group, each bumping the render chunks it overlaps. */
COOPA_TEST(change_tracking_covers_every_plane_and_chunk) {
    TerraData s = smooth();
    const TerraCursor base = s.cursor();

    s.mark_tiles_dirty(IRect{2, 3, 4, 5});
    const IRect r = s.dirty_since(base);
    expect(!r.empty() && r.x0 <= 2 && r.y0 <= 3 && r.x1 >= 4 && r.y1 >= 5, "the tile edit is reported, in tile space");
    expect(s.dirty_since(s.cursor()).empty(), "nothing is pending at the new cursor");

    const TerraCursor mid = s.cursor();
    s.mark_layer_dirty(IRect{0, 0, 4, 4});
    const IRect lr = s.dirty_since(mid);
    expect(lr.x0 == 0 && lr.y0 == 0 && lr.x1 >= 2 && lr.y1 >= 2, "a layer-only edit still comes back in tile space");

    const TerraCursor mid2 = s.cursor();
    const std::uint64_t before = s.chunk_revision(0, 0);
    s.mark_height_dirty(IRect{0, 0, 2, 2});
    expect(s.chunk_revision(0, 0) != before, "a height-only edit bumps the render chunk it touches");
    expect(s.dirty_since(mid2).x0 == 0 && s.dirty_since(mid2).y0 == 0, "...and the height edit converts to tile space too");

    TerraData t = terraced();
    const TerraCursor a = t.cursor();
    t.mark_tiles_dirty(IRect{1, 1, 2, 2});
    const TerraCursor b = t.cursor();
    t.mark_tiles_dirty(IRect{5, 5, 6, 6});
    expect(t.dirty_since(b) == IRect{5, 5, 6, 6}, "the newer cursor sees only the second edit");
    const IRect from_a = t.dirty_since(a);
    expect(from_a.x0 <= 1 && from_a.y0 <= 1 && from_a.x1 >= 6 && from_a.y1 >= 6, "the older cursor sees both");
}

/** @brief for_each_dirty_since() reports in tile space from every plane, oldest journal first. */
COOPA_TEST(for_each_dirty_since_reports_every_plane_in_tile_space) {
    TerraData s = smooth();
    const TerraCursor base = s.cursor();
    s.mark_tiles_dirty(IRect{0, 0, 1, 1});
    s.mark_height_dirty(IRect{s.height_nx() - 2, s.height_ny() - 2, s.height_nx(), s.height_ny()});

    IRect total;
    int calls = 0;
    s.for_each_dirty_since(base, [&](const IRect& r) { total.include(r); ++calls; });
    // Three disjoint regions, not two: the height edit also re-marks the TILES plane (it refreshes
    // TileCell::steps -- see mark_height_dirty's file comment), and that mark lands far enough
    // from the explicit tile edit at the origin that the journal doesn't coalesce the two.
    expect(calls == 3, "one callback per disjoint dirty region, across every plane that changed");
    expect(total.x0 == 0 && total.y0 == 0, "covers the tiles-plane edit");
    expect(total.x1 == s.geometry().tiles_x && total.y1 == s.geometry().tiles_y,
           "and the height edit at the far corner, both in tile space");
}

/**
 * @brief Raising height_sub from 1 expands every tile flat -- the lossless direction for a
 *        tile with no cliff at its border. (A genuine cliff has no representable shared corner
 *        between two tiles at different heights -- see terrain_field_test.cpp's Stage 0 cases,
 *        which falsify that specifically. This fixture is deliberately flat everywhere so the
 *        upsample has nothing to reconcile and the round trip is exact.)
 */
COOPA_TEST(reshape_can_add_a_height_plane_to_a_terraced_grid) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 8;
    g.tile_size = 1.0f;
    g.height_step = 0.5f;
    TerraData t(g);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) t.at(x, y).steps = 3; // flat: no cliff anywhere
    t.mark_all_dirty();

    TerraGeometry g2 = t.geometry();
    g2.height_sub = 4;
    t.reshape(g2);
    expect(t.has_heights(), "a height plane now exists");
    for (int ty = 0; ty < 8; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            for (int dy = 0; dy <= 4; ++dy)
                for (int dx = 0; dx <= 4; ++dx) {
                    expect_near(t.height_at(tx * 4 + dx, ty * 4 + dy), 1.5f, 1e-6f,
                               "every sample matches 3 steps * 0.5 m, corners included");
                }
        }
    }
}

/** @brief Reshaping a rectangular grid resamples each axis independently (no square constraint). */
COOPA_TEST(reshape_resamples_a_rectangular_height_plane) {
    TerraGeometry g;
    g.tiles_x = 4;
    g.tiles_y = 2;
    g.tile_size = 1.0f;
    g.height_sub = 4;
    TerraData d(g);
    for (int j = 0; j < d.height_ny(); ++j)
        for (int i = 0; i < d.height_nx(); ++i) d.height_at(i, j) = static_cast<float>(i); // a ramp along x only

    TerraGeometry g2 = g;
    g2.height_sub = 8;
    d.reshape(g2);
    expect(d.height_nx() == 4 * 8 + 1 && d.height_ny() == 2 * 8 + 1, "the new sub-resolution took effect");
    // Old texel 8 (value 8, the midpoint of a 17-sample row) sits at the same physical position
    // as new texel 16 (the midpoint of the 33-sample row): resampling by normalised index is
    // exactly resampling by physical position, since both grids span the same extent.
    expect_near(d.height_at(16, 0), 8.0f, 0.2f, "the ramp survives the resample, independent of the y axis' own sub");
}

COOPA_TEST(raycast_columns_steps_up_to_a_wall) {
    const TerraData t = terraced();
    toy::world::TileHit hit;
    // Shallow enough to clear the low terrace's flat ground (z stays positive through x < 4)
    // but still below the high terrace's 1.0 m top by the time it reaches x = 4, so it hits the
    // wall face rather than either terrace's flat top.
    const bool got = t.raycast_columns(glm::vec3(0.0f, 1.5f, 0.8f), glm::vec3(1.0f, 0.0f, -0.1f), 100.0f, hit);
    expect(got, "the ray finds the step from tile 0..3 up to tile 4..7");
    expect(hit.tile.x == 4, "it hits the wall at the terrace boundary");
}

COOPA_TEST(raycast_surface_requires_a_height_plane) {
    const TerraData t = terraced();
    float t_out = 0.0f;
    expect(!t.raycast_surface(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f, t_out),
           "no height plane: raycast_surface refuses rather than guessing");

    const TerraData s = smooth();
    expect(s.raycast_surface(glm::vec3(4.0f, 4.0f, 50.0f), glm::vec3(0.0f, 0.0f, -1.0f), 100.0f, t_out),
           "a height plane: the ray finds the surface");
}

COOPA_TEST(the_bake_handshake_matches_needs_bake_and_assign_baked) {
    TerraData t = terraced();
    expect(t.needs_bake(), "a freshly reset grid needs a bake");
    TerraData baked = terraced();
    baked.at(0, 0).steps = 9;
    t.assign_baked(std::move(baked));
    expect(!t.needs_bake(), "assign_baked() clears the flag");
    expect(t.at(0, 0).steps == 9, "and takes the baked content");
    expect(t.bakes_done() == 1, "bakes_done() counts it");
    t.request_bake();
    expect(t.needs_bake() && t.bake_requests() == 1, "request_bake() asks again");
}

// -------------------------------------------------------------------------------------------
// The redesign's single reconciliation chokepoint: `steps` is a maintained, derived cache of
// the height plane whenever one exists, refreshed as a side effect of mark_height_dirty() --
// never independently written by an edit path. These are the Stage 0 falsification cases
// (terrain_field_test.cpp's history), re-run here against the real chokepoint rather than a
// throwaway local reimplementation.

/** @brief A flat tile's steps survive an edit-then-refresh exactly, at every scale. */
COOPA_TEST(refresh_steps_is_exact_on_a_flat_tile) {
    const int height_sub = 8;
    const std::int16_t steps_values[] = {0, 1, 7, 120};
    const float height_steps[] = {0.1f, 1.0f, 2.5f};
    for (float hstep : height_steps) {
        for (std::int16_t steps : steps_values) {
            TerraGeometry g;
            g.tiles_x = g.tiles_y = 1;
            g.height_sub = height_sub;
            g.height_step = hstep;
            TerraData h(g);
            const float v = static_cast<float>(steps) * hstep;
            for (int dy = 0; dy <= height_sub; ++dy)
                for (int dx = 0; dx <= height_sub; ++dx) h.height_at(dx, dy) = v;
            h.mark_height_dirty(IRect{0, 0, h.height_nx(), h.height_ny()});
            expect(h.at(0, 0).steps == steps, "a flat tile's steps must survive the round trip exactly");
        }
    }
}

/**
 * @brief A cliff has no representable shared corner: the tile that writes a shared corner last
 * owns it, so refresh_steps is order-dependent at a cliff -- the empirical reason `steps` must
 * be a maintained cache (one chokepoint) rather than something every edit path re-derives.
 */
COOPA_TEST(a_cliffs_shared_corner_makes_refresh_steps_write_order_dependent) {
    const int height_sub = 2;
    const float hstep = 1.0f;
    TerraGeometry g;
    g.tiles_x = 2;
    g.tiles_y = 1;
    g.height_sub = height_sub;
    g.height_step = hstep;

    auto expand_flat = [&](TerraData& h, int tx, std::int16_t steps) {
        const float v = static_cast<float>(steps) * hstep;
        for (int dy = 0; dy <= height_sub; ++dy)
            for (int dx = 0; dx <= height_sub; ++dx) h.height_at(tx * height_sub + dx, dy) = v;
        h.mark_height_dirty(IRect{tx * height_sub, 0, tx * height_sub + height_sub + 1, height_sub + 1});
    };

    {
        TerraData h(g);
        expand_flat(h, 0, 3);   // low tile written first
        expand_flat(h, 1, 7);   // tall tile overwrites the shared corner
        expect(h.at(0, 0).steps == 4, "the low tile's mean is pulled toward its taller neighbour by the shared corner");
        expect(h.at(1, 0).steps == 7, "the tile that wrote the corner last reads back correctly");
    }
    {
        TerraData h(g);
        expand_flat(h, 1, 7);   // reverse the write order
        expand_flat(h, 0, 3);
        expect(h.at(0, 0).steps == 3, "reversing the order flips which tile owns the shared corner");
        expect(h.at(1, 0).steps == 6, "...and now the tall tile loses a step instead");
    }
}

/**
 * @brief Steps re-derived from an expansion of themselves must match -- the property that lets
 * refresh_steps run from every single height edit without drift accumulating stroke over stroke.
 */
COOPA_TEST(refresh_steps_is_idempotent_on_a_gentle_slope) {
    const int height_sub = 8;
    const int tiles_x = 4;
    const float hstep = 2.0f;
    TerraGeometry g;
    g.tiles_x = tiles_x;
    g.tiles_y = 1;
    g.height_sub = height_sub;
    g.height_step = hstep;

    TerraData h(g);
    for (int j = 0; j < h.height_ny(); ++j)
        for (int i = 0; i < h.height_nx(); ++i) h.height_at(i, j) = 0.5f * static_cast<float>(i);
    h.mark_height_dirty(IRect{0, 0, h.height_nx(), h.height_ny()});

    std::int16_t steps_1[4];
    for (int tx = 0; tx < tiles_x; ++tx) steps_1[tx] = h.at(tx, 0).steps;

    TerraData h2(g);
    for (int tx = 0; tx < tiles_x; ++tx) {
        const float v = static_cast<float>(steps_1[tx]) * hstep;
        for (int dy = 0; dy <= height_sub; ++dy)
            for (int dx = 0; dx <= height_sub; ++dx) h2.height_at(tx * height_sub + dx, dy) = v;
    }
    h2.mark_height_dirty(IRect{0, 0, h2.height_nx(), h2.height_ny()});

    for (int tx = 0; tx < tiles_x; ++tx) {
        expect(h2.at(tx, 0).steps == steps_1[tx], "re-deriving from the expansion must not drift");
    }
}
