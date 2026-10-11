/**
 * @file tile_map_test.cpp
 * @brief A TileTerrain's stored tiles (toyengine/world/tile_map_data.h): the 12-byte record,
 *        the `.tiles` file round trip (including a resave after an edit), the change journal
 *        consumers re-mesh from, the column raycast the editor hovers with, and the bake from a
 *        generated mapcoopa world (climate fields, water surfaces and depths, river flags).
 */

#include <coopa/testing/test.h>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_generator.h>
#include <toyengine/world/terrain_sampler.h>
#include <toyengine/world/tile_map_data.h>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("tile_map");

using namespace toy::test;
using toy::world::IRect;
using toy::world::TileKind;
using toy::world::TileMapData;
using toy::world::TileRecord;

namespace {

class QuietLogger : public coopa::debug::Logger {
public:
    QuietLogger() : Logger("tile_map_test") {}
    void info(const std::string&, std::string = "", bool = false, unsigned int = 0) override {}
};

/** @brief A deterministic, busy grid that does NOT divide into whole storage chunks. */
TileMapData make_grid(int w, int h) {
    TileMapData d;
    d.resize(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            TileRecord& r = d.at(x, y);
            r.steps = static_cast<std::int16_t>((x / 5 + y / 3) % 9);
            r.set_top_kind(static_cast<TileKind>((x / 7 + y / 4) % 12));
            r.set_side_kind(TileKind::Stone);
            r.biome = static_cast<std::uint8_t>((x + 2 * y) % 33);
            r.temperature = static_cast<std::uint8_t>(x * 3);
            r.moisture = static_cast<std::uint8_t>(y * 5);
            r.region = static_cast<std::uint16_t>(x / 16);
            r.set(TileRecord::Water, (x + y) % 11 == 0);
        }
    }
    return d;
}

bool same_records(const TileMapData& a, const TileMapData& b) {
    return a.width() == b.width() && a.height() == b.height() && a.records() == b.records();
}

} // namespace

COOPA_TEST(record_packs_kinds_and_flags_into_12_bytes) {
    TileRecord r;
    expect(r.top_kind() == TileKind::Grass && r.side_kind() == TileKind::Dirt, "a default record is grass over dirt");
    r.set_top_kind(TileKind::Salt);
    r.set_side_kind(TileKind::Ash);
    r.set(TileRecord::Water, true);
    r.set(TileRecord::River, true);
    r.set(TileRecord::Water, false);
    expect(r.top_kind() == TileKind::Salt && r.side_kind() == TileKind::Ash, "top and side kinds share a byte without bleeding");
    expect(!r.water() && r.has(TileRecord::River), "flags set and clear independently");
    expect(TileRecord::quantise01(1.0) == 255 && TileRecord::quantise01(0.0) == 0 && TileRecord::quantise01(2.0) == 255,
           "climate values quantise 0..1 onto a byte, clamped");
    r.steps = 7;
    const auto col = r.column();
    expect(col.steps == 7 && col.top_kind == TileKind::Salt && !col.water, "the record's column is what the mesher builds");
}

COOPA_TEST(tiles_file_round_trips_and_resaves_after_an_edit) {
    const auto path = (coopa::test::scratch_dir() / "world.tiles").string();
    TileMapData d = make_grid(70, 45);   // 3 x 2 storage chunks, the last ones partial
    expect(d.save(path), "the grid saves");
    expect(!d.dirty_on_disk(), "a save leaves it clean");

    TileMapData loaded;
    expect(loaded.load(path) && same_records(d, loaded), "loading gives back every record exactly");
    expect(!loaded.needs_bake() && !loaded.dirty_on_disk(), "a loaded grid needs no bake and has nothing unsaved");

    // Edit one tile in the last (partial) chunk; the resave recompresses that chunk only, and the
    // file still holds everything.
    loaded.at(69, 44).steps = 120;
    loaded.mark_dirty(IRect{69, 44, 70, 45});
    expect(loaded.dirty_on_disk(), "an edit is unsaved");
    expect(loaded.save(path), "the edited grid resaves");
    TileMapData again;
    expect(again.load(path) && same_records(loaded, again) && again.at(69, 44).steps == 120,
           "the resave holds the edit and every untouched chunk");

    const auto bytes = std::filesystem::file_size(path);
    expect(bytes < 70u * 45u * sizeof(TileRecord), "the file is compressed (" + std::to_string(bytes) + " bytes)");
    expect(!TileMapData().load(path + ".missing"), "a missing file does not load");
}

COOPA_TEST(journal_reports_edits_since_each_consumers_revision) {
    TileMapData d = make_grid(64, 64);
    const std::uint64_t r0 = d.revision();
    std::vector<IRect> seen;
    d.for_each_dirty_since(r0, [&](const IRect& r) { seen.push_back(r); });
    expect(seen.empty(), "nothing changed since the current revision");

    d.mark_dirty(IRect{2, 2, 4, 4});
    const std::uint64_t r1 = d.revision();
    d.mark_dirty(IRect{50, 50, 60, 60});
    expect(d.revision() > r1 && r1 > r0, "every edit bumps the revision");
    expect(d.dirty_since(r1) == (IRect{50, 50, 60, 60}), "a consumer at r1 sees only the later edit");
    IRect both = d.dirty_since(r0);
    expect(both.x0 == 2 && both.y0 == 2 && both.x1 == 60 && both.y1 == 60, "a consumer at r0 sees both");

    d.mark_dirty(IRect{-5, -5, 1000, 3});
    expect(d.dirty_since(d.revision() - 1) == (IRect{0, 0, 64, 3}), "edits are clipped to the grid");

    // Far more separate edits than the journal holds: a consumer that far behind is told "everything".
    for (int i = 0; i < 400; ++i) d.mark_dirty(IRect{(i * 37) % 60, (i * 53) % 60, (i * 37) % 60 + 1, (i * 53) % 60 + 1});
    expect(d.dirty_since(r0) == (IRect{0, 0, 64, 64}), "past the journal's reach, the whole grid is dirty");
}

COOPA_TEST(raycast_finds_tops_and_walls) {
    TileMapData d;
    d.resize(8, 8);
    for (const TileRecord& r : d.records()) (void)r;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) d.at(x, y).steps = 2;
    d.at(5, 3).steps = 6;   // a pillar

    toy::world::TileHit hit;
    expect(d.raycast(glm::vec3(2.5f, 2.5f, 20.0f), glm::vec3(0, 0, -1), 1.0f, 0.5f, 100.0f, hit), "a ray down hits");
    expect(hit.tile == glm::ivec2(2, 2) && std::abs(hit.point.z - 1.0f) < 1e-4f && hit.normal.z > 0.99f,
           "...the top of the tile below, at steps * height_step");

    // Level with the pillar's middle, along +x: enters its west wall.
    expect(d.raycast(glm::vec3(0.5f, 3.5f, 2.0f), glm::vec3(1, 0, 0), 1.0f, 0.5f, 100.0f, hit), "a level ray hits");
    expect(hit.tile == glm::ivec2(5, 3) && std::abs(hit.point.x - 5.0f) < 1e-4f && hit.normal.x < -0.99f,
           "...the pillar's wall it walks into, facing back at the ray");

    expect(!d.raycast(glm::vec3(-5.0f, 3.5f, 20.0f), glm::vec3(-1, 0, 0), 1.0f, 0.5f, 100.0f, hit), "a ray away from the grid misses");
}

COOPA_TEST(bake_stores_climate_water_and_rivers) {
    using namespace toy::world;
    QuietLogger logger;
    coopa::maps::MapConfig config;
    config.seed = 7;
    config.grid_size = 24;
    config.river_count = 12;
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();
    TerrainParams params = make_test_params(8);
    params.tiles_per_grid_unit = 3;
    params.height_scale = 20.0f;
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);

    TileMapData tiles;
    sampler.bake(tiles);
    expect(tiles.width() == 72 && tiles.height() == 72, "the bake covers the whole world");

    int water = 0, deep = 0, rivers = 0, ocean = 0;
    std::set<int> temperatures, biomes;
    bool columns_match = true;
    for (int y = 0; y < tiles.height(); ++y) {
        for (int x = 0; x < tiles.width(); ++x) {
            const TileRecord& r = tiles.at(x, y);
            temperatures.insert(r.temperature);
            biomes.insert(r.biome);
            if (r.water()) {
                ++water;
                if (r.water_depth > 0) ++deep;
                if (r.has(TileRecord::Ocean)) ++ocean;
            }
            if (r.has(TileRecord::River)) ++rivers;
            const TileColumn a = r.column(), b = sampler.sample(x, y);
            columns_match = columns_match && a.steps == b.steps && a.top_kind == b.top_kind && a.water == b.water;
        }
    }
    expect(columns_match, "the stored columns are exactly what the sampler builds");
    expect(water > 0 && ocean > 0, "the bake keeps water, and knows the ocean from lakes");
    expect(deep > 0, "water remembers the depth to its bed");
    expect(rivers > 0, "river channels are flagged");
    expect(temperatures.size() > 10 && biomes.size() > 3, "temperature and biome are stored per tile, and vary");
}
