/**
 * @file terra_image_test.cpp
 * @brief `terra_image.h`: a PNG decodes into whichever plane it targets, scaled correctly for a
 *        height map, resampled when the source is a different size than the destination plane,
 *        and the export/import round trip survives the 16-bit quantisation it must.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <toyengine/world/png16.h>
#include <toyengine/world/terra_data.h>
#include <toyengine/world/terra_image.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terra_image");

using toy::world::IRect;
using toy::world::TerraData;
using toy::world::TerraGeometry;

namespace {

TerraGeometry heightmap_geometry(int tiles, int height_sub, int layer_sub, float height) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = tiles;
    g.tile_size = 1.0f;
    g.height_sub = height_sub;
    g.layer_sub = layer_sub;
    g.height = height;
    g.height_step = 1.0f;
    g.chunk_tiles = tiles;
    return g;
}

} // namespace

COOPA_TEST(a_same_size_height_png_lands_at_full_precision) {
    // 4 tiles, height_sub 2: a 9 x 9 plane -- deliberately not a power of two, so an off-by-one
    // in the resample math would show up as a wrong corner rather than cancelling out.
    TerraData d(heightmap_geometry(4, 2, 1, 100.0f));
    const int n = d.height_nx();
    require(n == 9, "fixture: 4 tiles at height_sub 2 is a 9-sample plane");

    std::vector<std::uint16_t> px(static_cast<std::size_t>(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) px[static_cast<std::size_t>(y) * n + x] = static_cast<std::uint16_t>((x + y * n) * 700);
    const auto path = (coopa::test::scratch_dir() / "terra_image_same_size.png").string();
    require(toy::world::write_png16_grey(path, n, n, px), "fixture: the synthetic heightmap PNG writes");

    const auto cursor_before = d.cursor();
    expect(toy::world::import_height_png(path, d), "import: a same-size 16-bit PNG imports");
    expect(d.cursor().heights != cursor_before.heights, "import: the height plane's journal moved");

    float max_diff = 0.0f;
    int worst_x = 0, worst_y = 0;
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float expect_m = static_cast<float>(px[static_cast<std::size_t>(y) * n + x]) / 65535.0f * 100.0f;
            const float diff = std::fabs(d.height_at(x, y) - expect_m);
            if (diff > max_diff) { max_diff = diff; worst_x = x; worst_y = y; }
        }
    }
    expect(max_diff <= 1e-3f, "import: worst sample (" + std::to_string(worst_x) + "," + std::to_string(worst_y) +
                                  ") off by " + std::to_string(max_diff) + " m");
}

COOPA_TEST(export_then_import_round_trips_within_one_quantisation_step) {
    TerraData d(heightmap_geometry(3, 4, 1, 64.0f)); // 13-sample plane, an analytic ramp
    const int n = d.height_nx();
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            d.height_at(x, y) = (static_cast<float>(x) / static_cast<float>(n - 1)) * d.geometry().height;

    const auto path = (coopa::test::scratch_dir() / "terra_image_roundtrip.png").string();
    expect(toy::world::export_height_png(path, d), "export: the height plane writes a PNG");

    TerraData reimported(heightmap_geometry(3, 4, 1, 64.0f));
    expect(toy::world::import_height_png(path, reimported), "import: the exported PNG reads back");

    // One quantisation step at this scale: height / 65535.
    const float step = d.geometry().height / 65535.0f;
    float max_diff = 0.0f;
    for (int x = 0; x < n; ++x) max_diff = std::max(max_diff, std::fabs(reimported.height_at(x, 0) - d.height_at(x, 0)));
    expect(max_diff <= step * 1.5f, "roundtrip: drifted " + std::to_string(max_diff) +
                                         " m, over one quantisation step (" + std::to_string(step) + " m)");
}

COOPA_TEST(a_different_sized_source_is_resampled_not_rejected) {
    // A tiny 3x3 "DEM" imported into a much finer plane -- the whole point of resampling: any
    // heightmap works with any terrain, not just one authored at the exact matching resolution.
    const std::vector<std::uint16_t> src = {0, 0, 65535, 0, 32768, 65535, 65535, 65535, 65535};
    const auto path = (coopa::test::scratch_dir() / "terra_image_small_source.png").string();
    require(toy::world::write_png16_grey(path, 3, 3, src), "fixture: the 3x3 source PNG writes");

    TerraData d(heightmap_geometry(8, 4, 1, 200.0f)); // 33-sample plane
    expect(toy::world::import_height_png(path, d), "import: a smaller source PNG still imports");

    const int n = d.height_nx();
    // Corners are shared between source and destination regardless of resample ratio.
    expect(d.height_at(0, 0) < 1.0f, "resample: the source's dark corner (0,0) stays near 0");
    expect(std::fabs(d.height_at(n - 1, 0) - 200.0f) < 1.0f, "resample: the source's bright corner (2,0) stays near max");
    expect(std::fabs(d.height_at(n - 1, n - 1) - 200.0f) < 1.0f, "resample: the source's bright corner (2,2) stays near max");
}

COOPA_TEST(importing_a_height_png_without_a_height_plane_fails_cleanly) {
    TerraData flat(heightmap_geometry(4, 1, 1, 100.0f)); // height_sub 1: no height plane at all
    expect(!flat.has_heights(), "fixture: height_sub 1 means no height plane");
    const auto path = (coopa::test::scratch_dir() / "terra_image_unused.png").string();
    const std::vector<std::uint16_t> px(4, 0);
    require(toy::world::write_png16_grey(path, 2, 2, px), "fixture: the PNG writes");
    expect(!toy::world::import_height_png(path, flat), "import: refuses a field with no height plane");
}

COOPA_TEST(a_layer_png_lands_in_the_right_plane_with_the_right_channels) {
    TerraData d(heightmap_geometry(2, 2, 2, 50.0f)); // layer plane: 4 x 4 texels
    // Four quadrants, each a distinct RGBA so a channel swap or a transposed axis shows up.
    std::vector<std::uint8_t> rgba(4 * 4 * 4, 0);
    const auto set = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
        const std::size_t i = (static_cast<std::size_t>(y) * 4 + x) * 4;
        rgba[i + 0] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a;
    };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) set(x, y, static_cast<std::uint8_t>(x * 60), static_cast<std::uint8_t>(y * 60), 10, 255);
    const auto path = (coopa::test::scratch_dir() / "terra_image_layer.png").string();
    require(toy::world::write_png_rgba8(path, 4, 4, rgba), "fixture: the splat PNG writes");

    expect(toy::world::import_layer_png(path, d, 1), "import: a layer PNG imports into plane 1");
    // Untouched by importing into plane 1: reset()'s default fill_layer(0) left layer 0 (plane
    // 0's R channel) at full weight everywhere.
    expect(d.layer_at(0, 0, 0) == 255, "import: plane 0 is untouched by an import targeting plane 1");

    const std::uint8_t weight = d.layer_at(4, 0, 0); // layer 4 == plane 1's channel 0 (R)
    expect(weight < 30, "import: texel (0,0)'s R channel matches the source's dark corner");
    expect(!toy::world::import_layer_png(path, d, 2), "import: rejects a plane index outside 0/1");
}
