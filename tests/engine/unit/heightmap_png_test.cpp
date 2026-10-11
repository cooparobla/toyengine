/**
 * @file heightmap_png_test.cpp
 * @brief Heightmap files and the store, device-free: the 16-bit PNG writer round-trips exactly,
 *        8-bit images import, heightmap / splat save and load round-trip, and the store keeps
 *        live (unsaved) data across re-acquisition and restores an earlier shape exactly.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <toyengine/world/heightmap_store.h>
#include <toyengine/world/png16.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("heightmap_png");

using namespace toy::world;

namespace {

std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "toyengine_heightmap_png_test";
    std::filesystem::create_directories(dir);
    return dir / name;
}

HeightmapGeometry small_geometry() {
    HeightmapGeometry g;
    g.size = glm::vec2(64.0f);
    g.height = 30.0f;
    g.chunk_size = 16.0f;
    g.resolution = 33;
    g.splat_resolution = 16;
    return g;
}

} // namespace

/** @brief 16-bit greyscale written by our encoder reads back bit-exact through stb. */
COOPA_TEST(png16_round_trip_is_exact) {
    const int w = 37, h = 21;
    std::vector<std::uint16_t> px(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint16_t>((i * 2654435761u) >> 7);
    const std::string path = scratch("rt16.png").string();
    require(write_png16_grey(path, w, h, px), "png16: written");
    int rw = 0, rh = 0;
    std::vector<std::uint16_t> back;
    require(read_png16_grey(path, rw, rh, back), "png16: read back");
    expect(rw == w && rh == h, "png16: same size");
    expect(back == px, "png16: every sample identical");
}

/** @brief An 8-bit image imports as a heightmap (stb widens it), scaled to the height. */
COOPA_TEST(eight_bit_heightmaps_import) {
    const std::vector<std::uint8_t> rgba(4 * 4 * 4, 255);   // opaque white
    const std::string path = scratch("white8.png").string();
    require(write_png_rgba8(path, 4, 4, rgba), "png16: 8-bit image written");
    HeightmapData d(small_geometry());
    require(load_heightmap_image(d, path), "png16: 8-bit imported");
    expect_near(d.at(10, 20), 30.0f, 1e-3f, "png16: white is the full height");
}

/** @brief Heights and splats survive save -> load at 16-bit precision. */
COOPA_TEST(heightmap_and_splat_round_trip) {
    HeightmapData d(small_geometry());
    HeightmapGenerate gen;
    gen.seed = 3;
    d.generate(gen);
    d.splat_at(2, 5, 6) = 128;
    d.normalize_splat_texel(5, 6, 2);
    const std::string hp = scratch("h.png").string(), s0 = scratch("s0.png").string(), s1 = scratch("s1.png").string();
    require(save_heightmap_image(d, hp) && save_splat_image(d, 0, s0) && save_splat_image(d, 1, s1), "png16: saved");
    HeightmapData e(small_geometry());
    require(load_heightmap_image(e, hp) && load_splat_image(e, 0, s0) && load_splat_image(e, 1, s1), "png16: loaded");
    float worst = 0.0f;
    for (std::size_t i = 0; i < d.heights().size(); ++i) worst = std::max(worst, std::abs(d.heights()[i] - e.heights()[i]));
    expect(worst <= 30.0f / 65535.0f + 1e-5f, "png16: heights within one 16-bit step");
    expect(e.splat(0) == d.splat(0) && e.splat(1) == d.splat(1), "png16: splats identical");
}

/** @brief The store hands back the same live data, keeps unsaved edits, and restores shapes exactly. */
COOPA_TEST(store_keeps_live_data_and_shapes) {
    HeightmapStore& store = HeightmapStore::instance();
    store.clear();
    HeightmapFiles files;
    files.height = scratch("store_h.png").string();
    files.splat0 = scratch("store_s0.png").string();
    files.splat1 = scratch("store_s1.png").string();
    std::filesystem::remove(files.height);
    HeightmapGenerate gen;
    gen.seed = 11;
    auto a = store.acquire(files, small_geometry(), gen);
    a->at(3, 3) = 99.0f;
    a->mark_height_dirty(IRect{3, 3, 4, 4});
    auto b = store.acquire(files, small_geometry(), gen);
    expect(a == b, "store: same object on re-acquire");
    expect_near(b->at(3, 3), 99.0f, 1e-6f, "store: unsaved edit kept");

    const std::vector<float> before = b->heights();
    HeightmapGeometry bigger = small_geometry();
    bigger.resolution = 65;
    store.acquire(files, bigger, gen);
    expect(b->nx() == 65, "store: reshaped in place");
    store.acquire(files, small_geometry(), gen);
    expect(b->nx() == 33 && b->heights() == before, "store: the earlier shape restored exactly");

    require(store.save(b.get()), "store: saved");
    expect(!b->dirty_on_disk() && std::filesystem::exists(files.height), "store: written and clean");
    store.clear();
    auto c = store.acquire(files, small_geometry(), gen);
    expect_near(c->at(3, 3), std::min(99.0f, 30.0f), 30.0f / 65535.0f + 1e-4f,
                "store: a fresh load reads the saved file (clamped to the height)");
    store.clear();
}
