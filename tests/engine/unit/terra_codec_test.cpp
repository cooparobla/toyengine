/**
 * @file terra_codec_test.cpp
 * @brief The `.terra` file (toyengine/world/terra_codec.h): byte-exact round trip, incremental
 *        re-save (unchanged chunks produce byte-identical blobs, by deflate's determinism, with
 *        no persistent cache needed), forward-compatible skip of an unrecognised plane, and an
 *        atomic load that leaves its target untouched on any corruption.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <toyengine/world/terra_codec.h>
#include <toyengine/world/terra_data.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terra_codec");

using toy::world::IRect;
using toy::world::TerraData;
using toy::world::TerraGeometry;
using toy::world::TileCell;
using toy::world::TileKind;
using toy::world::WaterKind;

namespace {

/** @brief Deterministic, non-uniform content across every plane -- nothing here is flat, so a
 *         byte-for-byte comparison actually exercises the transpose/compress round trip. */
TerraData filled(const TerraGeometry& g) {
    TerraData d(g);
    for (int y = 0; y < g.tiles_y; ++y) {
        for (int x = 0; x < g.tiles_x; ++x) {
            TileCell& c = d.at(x, y);
            c.steps = static_cast<std::int16_t>((x * 7 + y * 3) % 23 - 5);
            c.set_top_kind(static_cast<TileKind>((x + y) % 11));
            c.biome = static_cast<std::uint8_t>((x * 13 + y) % 251);
            c.temperature = static_cast<std::uint8_t>((x * 31 + y * 17) % 255);
            if ((x + y) % 5 == 0) {
                c.water_kind = static_cast<std::uint8_t>(WaterKind::Lake);
                c.water_level = static_cast<float>(x) * 0.37f + 1.0f;
            }
            if ((x + y) % 7 == 0) {
                c.water_kind = static_cast<std::uint8_t>(WaterKind::River);
                c.flow = static_cast<std::uint8_t>((x % 8) + 1);
                c.flow_authored = (x % 3 == 0) ? 1 : 0;
            }
        }
    }
    if (d.has_heights()) {
        for (int j = 0; j < d.height_ny(); ++j)
            for (int i = 0; i < d.height_nx(); ++i) d.height_at(i, j) = std::sin(0.1f * i) * 3.0f + 0.05f * j;
    }
    for (int j = 0; j < d.layer_ny(); ++j) {
        for (int i = 0; i < d.layer_nx(); ++i) {
            const int l = (i * 3 + j * 5) % 8;
            d.layer_at(l, i, j) = 255;
        }
    }
    d.mark_all_dirty();
    return d;
}

bool planes_equal(const TerraData& a, const TerraData& b) {
    if (a.geometry() != b.geometry()) return false;
    if (a.tiles() != b.tiles()) return false;
    if (a.has_heights() != b.has_heights()) return false;
    if (a.has_heights() && a.heights() != b.heights()) return false;
    if (a.layer_plane(0) != b.layer_plane(0)) return false;
    if (a.layer_plane(1) != b.layer_plane(1)) return false;
    return true;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

/** @brief The byte range of one plane's chunk `c`'s blob, by its id (1 Tiles, 3 Layer0, 4 Layer1). */
std::pair<std::size_t, std::size_t> chunk_blob_range(const std::vector<std::uint8_t>& bytes, std::uint32_t id,
                                                      std::uint32_t chunk) {
    const std::uint32_t plane_count = get_u32(bytes.data() + 20);
    for (std::uint32_t pi = 0; pi < plane_count; ++pi) {
        const std::uint8_t* e = bytes.data() + 64 + static_cast<std::size_t>(pi) * 28;
        if (get_u32(e) != id) continue;
        const std::uint64_t index_off = get_u64(e + 20);
        const std::uint8_t* idx = bytes.data() + index_off + static_cast<std::size_t>(chunk) * 16;
        const std::uint64_t off = get_u64(idx);
        const std::uint32_t size = get_u32(idx + 8);
        return {static_cast<std::size_t>(off), static_cast<std::size_t>(size)};
    }
    return {0, 0};
}

bool blob_unchanged(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, std::uint32_t id,
                    std::uint32_t chunk) {
    const auto ra = chunk_blob_range(a, id, chunk);
    const auto rb = chunk_blob_range(b, id, chunk);
    if (ra.second != rb.second) return false;
    return std::memcmp(a.data() + ra.first, b.data() + rb.first, ra.second) == 0;
}

} // namespace

/** @brief Round-trip exactness across the shapes the file format must not special-case. */
COOPA_TEST(round_trip_is_byte_exact_across_shapes) {
    struct Case {
        int tx, ty, hsub, lsub;
        const char* name;
    };
    const Case cases[] = {
        {8, 8, 1, 1, "no height plane, square"},
        {20, 6, 4, 2, "rectangular, with a height plane"},
        {40, 40, 1, 1, "two full storage chunks"},
        {33, 17, 2, 1, "not a multiple of the storage chunk"},
        {5, 5, 1, 1, "smaller than one storage chunk"},
    };
    for (const Case& tc : cases) {
        TerraGeometry g;
        g.tiles_x = tc.tx;
        g.tiles_y = tc.ty;
        g.tile_size = 1.5f;
        g.height_sub = tc.hsub;
        g.layer_sub = tc.lsub;
        g.height_step = 0.25f;
        g.height = 40.0f;
        g.chunk_tiles = 16;
        const TerraData src = filled(g);

        const auto path = (coopa::test::scratch_dir() / (std::string("rt_") + tc.name + ".terra")).string();
        require(save_terra(path, src), tc.name);

        TerraData loaded;
        require(load_terra(path, loaded), tc.name);
        expect(planes_equal(src, loaded), tc.name);

        // Saving the loaded copy again must reproduce the exact same file bytes.
        const auto path2 = path + ".2";
        require(save_terra(path2, loaded), tc.name);
        expect(read_file(path) == read_file(path2), tc.name);
    }
}

/** @brief An edit confined to one storage chunk changes only that chunk's bytes on resave. */
COOPA_TEST(resaving_after_one_edit_changes_only_its_own_chunk) {
    TerraGeometry g;
    g.tiles_x = 64; // exactly two 32-tile storage chunks
    g.tiles_y = 32;
    g.tile_size = 1.0f;
    TerraData d = filled(g);
    const auto path = (coopa::test::scratch_dir() / "incremental.terra").string();
    require(save_terra(path, d), "first save");
    const auto before = read_file(path);

    d.at(5, 5).steps = static_cast<std::int16_t>(d.at(5, 5).steps + 1); // inside storage chunk (0, 0)
    d.mark_tiles_dirty(IRect{5, 5, 6, 6});
    require(save_terra(path, d), "second save");
    const auto after = read_file(path);

    expect(!blob_unchanged(before, after, 1, 0), "the tiles plane's chunk (0,0), which the edit landed in, changed");
    expect(blob_unchanged(before, after, 1, 1), "the tiles plane's chunk (1,0) is untouched -- same bytes in, same deflate out");
    expect(blob_unchanged(before, after, 3, 0) && blob_unchanged(before, after, 3, 1), "layer0's chunks are untouched");
    expect(blob_unchanged(before, after, 4, 0) && blob_unchanged(before, after, 4, 1), "layer1's chunks are untouched");
}

/** @brief An unrecognised plane id is skipped, not fatal -- the loader still recovers everything
 *         it understands. A hand-built file lets us add one without going through save_terra. */
COOPA_TEST(an_unknown_plane_is_skipped_not_fatal) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 4;
    TerraData src = filled(g);
    const auto path = (coopa::test::scratch_dir() / "forward_compat.terra").string();
    require(save_terra(path, src), "save");

    std::vector<std::uint8_t> bytes = read_file(path);
    // plane_count lives at header offset 20; the table starts at 64 and each entry is 28 bytes.
    // Append one garbage entry right after the real table -- which shifts every real plane's
    // index+blob data 28 bytes later in the file. Two kinds of stored position go stale because
    // of that and both need patching: each real entry's own `index_off` (where ITS chunk index
    // starts), and -- inside each of those now-relocated chunk indices -- every chunk's `offset`
    // (where ITS blob starts), since the blob bytes themselves moved by the same 28 bytes. The
    // bogus entry's OWN index_off points past the file on purpose: if the loader ever followed
    // it (it must not, for an id it doesn't recognise), this test would crash instead of just
    // failing.
    const std::uint32_t old_count = static_cast<std::uint32_t>(bytes[20]) | (static_cast<std::uint32_t>(bytes[21]) << 8) |
                                    (static_cast<std::uint32_t>(bytes[22]) << 16) | (static_cast<std::uint32_t>(bytes[23]) << 24);
    bytes[20] = static_cast<std::uint8_t>((old_count + 1) & 0xFF);
    auto patch_u64 = [&](std::size_t at, std::uint64_t delta) {
        std::uint8_t* p = bytes.data() + at;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        v += delta;
        for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    const int scx = (g.tiles_x + 31) / 32, scy = (g.tiles_y + 31) / 32;
    const std::uint32_t chunk_count = static_cast<std::uint32_t>(scx) * static_cast<std::uint32_t>(scy);
    for (std::uint32_t pi = 0; pi < old_count; ++pi) {
        const std::size_t entry = 64 + static_cast<std::size_t>(pi) * 28;
        std::uint64_t index_off = 0;
        for (int i = 0; i < 8; ++i) index_off |= static_cast<std::uint64_t>(bytes[entry + 20 + i]) << (8 * i);
        for (std::uint32_t c = 0; c < chunk_count; ++c) patch_u64(index_off + static_cast<std::size_t>(c) * 16, 28);
        patch_u64(entry + 20, 28);
    }
    std::vector<std::uint8_t> bogus_entry(28, 0);
    bogus_entry[0] = 99; // id: not Tiles(1)/Height(2)/Layer0(3)/Layer1(4)
    for (int i = 0; i < 8; ++i) bogus_entry[20 + i] = 0xFF; // index_off: absurdly far past EOF
    const auto insert_at = bytes.begin() + 64 + static_cast<std::ptrdiff_t>(old_count) * 28; // right after the real table
    bytes.insert(insert_at, bogus_entry.begin(), bogus_entry.end());

    const auto tampered = (coopa::test::scratch_dir() / "forward_compat_tampered.terra").string();
    std::ofstream out(tampered, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();

    TerraData loaded;
    expect(load_terra(tampered, loaded), "a file with one extra, unrecognised plane still loads");
    expect(planes_equal(src, loaded), "and every plane this engine understands comes back exactly");
}

/** @brief A corrupted blob is rejected, and the load is atomic: the target is left untouched. */
COOPA_TEST(a_corrupted_blob_is_rejected_and_the_target_stays_untouched) {
    TerraGeometry g;
    g.tiles_x = g.tiles_y = 4;
    const TerraData src = filled(g);
    const auto path = (coopa::test::scratch_dir() / "corrupt.terra").string();
    require(save_terra(path, src), "save");

    std::vector<std::uint8_t> bytes = read_file(path);
    // Flip a byte in the middle of the file -- inside some plane's compressed blob -- so its
    // decompressed length can no longer match the raw_size recorded for it.
    bytes[bytes.size() / 2] ^= 0xFF;
    const auto tampered = path + ".bad";
    std::ofstream out(tampered, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();

    TerraGeometry sentinel_g;
    sentinel_g.tiles_x = sentinel_g.tiles_y = 2;
    TerraData target(sentinel_g);
    target.at(0, 0).steps = 123; // a recognisable sentinel the failed load must not touch
    const bool ok = load_terra(tampered, target);
    expect(!ok, "a corrupted blob fails to load");
    expect(target.geometry().tiles_x == 2 && target.at(0, 0).steps == 123, "and the target is untouched by the failed attempt");
}
