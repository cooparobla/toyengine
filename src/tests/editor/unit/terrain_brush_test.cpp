/**
 * @file terrain_brush_test.cpp
 * @brief The heightmap terrain brushes on a known grid: Raise lifts only inside the brush (and Ctrl
 *        lowers), Smooth reduces a spike, Flatten and Set Height converge on their targets, Paint
 *        keeps every texel's weights summing to 255 (and Ctrl erases), heights stay inside
 *        [0, height], and an undo patch restores the exact grid.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "editor/terrain/terrain_brush.h"

COOPA_TEST_SUITE("terrain_brush");

namespace toy::editor::testing {

using coopa::test::expect;
using coopa::test::expect_near;
using toy::world::IRect;
using toy::world::TerraData;
using toy::world::TerraGeometry;

namespace {

/**
 * @brief 64 x 64 m, 32 tiles a side, height_sub 2: a 65-sample plane with exactly 1 m between
 *        samples, so a sample's grid index equals its local-metre position (as the hardcoded
 *        coordinates below assume).
 */
std::shared_ptr<TerraData> flat(float h) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 32;
    g.tile_size = 2.0f;
    g.height = 40.0f;
    g.height_sub = 2;
    g.layer_sub = 1;
    g.chunk_tiles = 8;
    auto d = std::make_shared<TerraData>(g);
    std::fill(d->heights().begin(), d->heights().end(), h);
    d->mark_all_dirty();
    return d;
}

} // namespace

COOPA_TEST(raise_and_lower_stay_inside_the_brush) {
    auto d = flat(10.0f);
    TerrainBrushSettings s;
    s.tool = TerrainTool::Raise;
    s.radius = 8.0f;
    s.strength = 1.0f;
    const IRect r = terrain_dab(*d, s, glm::vec2(32.0f), false);
    expect(!r.empty(), "terrain brush: the dab reports its texels");
    expect(d->height_at(32, 32) > 10.0f, "terrain brush: Raise lifts the centre");
    expect_near(d->height_at(32 + 9, 32), 10.0f, 1e-6f, "terrain brush: ...and nothing outside the radius");
    const float raised = d->height_at(32, 32);
    terrain_dab(*d, s, glm::vec2(32.0f), true);
    expect(d->height_at(32, 32) < raised, "terrain brush: Ctrl lowers");
    s.radius = 30.0f;
    for (int k = 0; k < 200; ++k) terrain_dab(*d, s, glm::vec2(32.0f), false);
    expect(d->height_range().y <= 40.0f, "terrain brush: never above the terrain's height");
}

COOPA_TEST(smooth_flatten_and_set_height) {
    auto d = flat(10.0f);
    d->height_at(20, 20) = 30.0f;
    TerrainBrushSettings s;
    s.radius = 4.0f;
    s.strength = 1.0f;
    s.tool = TerrainTool::Smooth;
    terrain_dab(*d, s, glm::vec2(20.0f), false);
    expect(d->height_at(20, 20) < 15.0f, "terrain brush: Smooth knocks a spike down");
    s.tool = TerrainTool::Flatten;
    for (int k = 0; k < 6; ++k) terrain_dab(*d, s, glm::vec2(20.0f), false, 12.0f);
    expect_near(d->height_at(20, 20), 12.0f, 0.05f, "terrain brush: Flatten converges on its height");
    s.tool = TerrainTool::SetHeight;
    s.target_height = 25.0f;
    for (int k = 0; k < 8; ++k) terrain_dab(*d, s, glm::vec2(40.0f), false);
    expect_near(d->height_at(40, 40), 25.0f, 0.05f, "terrain brush: Set Height converges on the target");
}

COOPA_TEST(paint_keeps_weights_normalised) {
    auto d = flat(5.0f);
    TerrainBrushSettings s;
    s.tool = TerrainTool::Paint;
    s.radius = 10.0f;
    s.strength = 0.7f;
    s.layer = 5;
    for (int k = 0; k < 3; ++k) terrain_dab(*d, s, glm::vec2(32.0f), false);
    bool sums = true;
    for (int j = 0; j < d->layer_ny(); ++j) {
        for (int i = 0; i < d->layer_nx(); ++i) {
            int sum = 0;
            for (int l = 0; l < 8; ++l) sum += d->layer_at(l, i, j);
            sums = sums && sum == 255;
        }
    }
    expect(sums, "terrain brush: every texel's weights sum to 255");
    expect(d->layer_weight(5, glm::vec2(32.0f)) > 0.9f, "terrain brush: the painted layer dominates the centre");
    expect(d->layer_weight(5, glm::vec2(2.0f)) < 0.01f, "terrain brush: ...and is absent far away");
    for (int k = 0; k < 6; ++k) terrain_dab(*d, s, glm::vec2(32.0f), true);
    expect(d->layer_weight(5, glm::vec2(32.0f)) < 0.2f, "terrain brush: Ctrl erases it");
    expect(d->layer_weight(0, glm::vec2(32.0f)) > 0.8f, "terrain brush: ...and the base layer comes back");
}

COOPA_TEST(undo_patches_restore_exactly) {
    auto d = flat(10.0f);
    const std::vector<float> start = d->heights();
    TerrainBrushSettings s;
    s.tool = TerrainTool::Noise;
    s.radius = 12.0f;
    s.strength = 1.0f;
    IRect r = terrain_dab(*d, s, glm::vec2(30.0f), false, 0.0f, 99);
    r.include(terrain_dab(*d, s, glm::vec2(36.0f), false, 0.0f, 99));
    expect(d->heights() != start, "terrain brush: Noise changed the grid");
    const TerrainPatch before = capture_patch_from(d, start, d->layer_plane(0), d->layer_plane(1), r, IRect{});
    const TerrainPatch after = capture_patch(d, r, IRect{});
    expect(apply_patch(before) && d->heights() == start, "terrain brush: the before patch restores exactly");
    expect(apply_patch(after) && d->heights() != start, "terrain brush: the after patch redoes");
    TerraGeometry g = d->geometry();
    g.tiles_x = g.tiles_y = 16;   // a different resolution
    d->reshape(g);
    expect(!apply_patch(before), "terrain brush: a patch from another resolution is refused");
}

} // namespace toy::editor::testing
