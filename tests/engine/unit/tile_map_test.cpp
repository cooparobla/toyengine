/**
 * @file tile_map_test.cpp
 * @brief A TileTerrain's stored tiles (toyengine/world/tile_map_data.h): the 16-byte record,
 *        the `.tiles` file round trip (including a resave after an edit, and a v1 file's
 *        conversion), the change journal consumers re-mesh from, the column raycast the editor
 *        hovers with, the bake from a generated mapcoopa world (climate fields, water that never
 *        stands over dry ground, river flags), water editing (tile_water.h), and the
 *        `generator:` block (terrain_generator.h).
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
#include <toyengine/world/tile_water.h>
#include <toyengine/world/terrain_generator.h>
#include <coopa/maps/map_presets.h>
#include <coopa/maps/map_yaml.h>
#include <cstdlib>
#include <fstream>

#include "engine/support/checks.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("tile_map");

using namespace toy::test;
using toy::world::IRect;
using toy::world::TileKind;
using toy::world::TileMapData;
using toy::world::TileRecord;

// Defined (with C linkage) by the stb_image_write implementation gfxcoopa compiles once.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

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
            if ((x + y) % 11 == 0) r.set_water(r.steps + 2, (x + y) % 22 == 0);
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

    int water = 0, rivers = 0, ocean = 0, flowing = 0, over = 0, shallow_bed = 0;
    std::set<int> temperatures, biomes;
    for (int y = 0; y < tiles.height(); ++y) {
        for (int x = 0; x < tiles.width(); ++x) {
            const TileRecord& r = tiles.at(x, y);
            temperatures.insert(r.temperature);
            biomes.insert(r.biome);
            if (r.has(TileRecord::River)) ++rivers;
            if (!r.water()) continue;
            ++water;
            if (r.has(TileRecord::Ocean)) ++ocean;
            if (r.has(TileRecord::Flowing)) ++flowing;
            if (r.water_level() - r.steps < 1) ++shallow_bed;
            // No dry tile beside water stands lower than that water's terrace.
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (tiles.contains(x + dx, y + dy) && !tiles.at(x + dx, y + dy).water() &&
                        tiles.at(x + dx, y + dy).steps < r.water_level())
                        ++over;
        }
    }
    expect(water > 0 && ocean > 0, "the bake keeps water, and knows the ocean from lakes");
    expect(flowing > 0, "rivers carry water of their own (" + std::to_string(flowing) + " tiles)");
    expect(over == 0, "no dry tile stands below the water beside it (" + std::to_string(over) + " do)");
    expect(shallow_bed == 0, "every bed is at least a step under its water");
    expect(rivers > 0, "river channels are flagged");
    expect(temperatures.size() > 10 && biomes.size() > 3, "temperature and biome are stored per tile, and vary");
}

COOPA_TEST(world_size_is_chosen_apart_from_the_map) {
    using namespace toy::world;
    QuietLogger logger;
    coopa::maps::MapConfig config;
    config.seed = 7;
    config.grid_size = 24;
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();
    TerrainParams params = make_test_params(8);
    params.tiles_per_grid_unit = 3;
    params.world_tiles = 100;   // 4.17 tiles per cell: wins over tiles_per_grid_unit
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);
    TileMapData tiles;
    sampler.bake(tiles);
    expect(tiles.width() == 100 && tiles.height() == 100, "world_tiles sets the world's size in tiles");
    int water = 0;
    for (const TileRecord& r : tiles.records()) water += r.water() ? 1 : 0;
    expect(water > 0 && water < 100 * 100, "...and the whole map is cut into it, land and sea");
    expect(params.tiles_for_grid(24) == 100 && TerrainParams{}.tiles_for_grid(24) == 96,
           "0 derives it from the cells: 24 x 4");
}

COOPA_TEST(a_v1_file_loads_as_ground_and_water) {
    // A v1 file: 12-byte records, a water tile standing at its surface with the bed below.
    TileMapData d;
    d.resize(3, 2);
    d.at(1, 0).steps = 6;
    d.at(1, 0).set_water(9, true);   // ground 6, water to 9
    d.at(2, 1).steps = 4;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "toy_tile_map_v1";
    std::filesystem::create_directories(dir);
    const std::string v2 = (dir / "v2.tiles").string(), v1 = (dir / "v1.tiles").string();
    expect(d.save(v2), "saves");
    // Hand-write the v1 form of the same grid: version 1, record size 12, water at its surface.
    {
        TileMapData old;
        old.resize(3, 2);
        old.at(1, 0) = d.at(1, 0);
        old.at(1, 0).steps = 9;          // v1: the column stood at the surface
        old.at(1, 0).water_depth = 3;    // ...with the bed 3 below
        old.at(2, 1).steps = 4;
        std::vector<std::uint8_t> raw;
        for (int field = 0; field < 12; ++field)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 3; ++x) raw.push_back(reinterpret_cast<const std::uint8_t*>(&old.at(x, y))[field]);
        int len = 0;
        unsigned char* z = stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &len, 8);
        std::vector<std::uint8_t> file = {'T', 'T', 'I', 'L'};
        auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) file.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
        u32(1); u32(3); u32(2); u32(32); u32(12); u32(1); u32(0); u32(static_cast<std::uint32_t>(len));
        file.insert(file.end(), z, z + len);
        std::free(z);
        std::ofstream out(v1, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    TileMapData loaded;
    expect(loaded.load(v1), "a v1 file loads");
    const TileRecord& w = loaded.at(1, 0);
    expect(w.steps == 6 && w.water_level() == 9 && w.water() && w.has(TileRecord::Ocean),
           "its water tile becomes a bed under water to the same surface");
    expect(w.top_kind() == TileKind::Sand, "...with a sand bed");
    expect(loaded.at(2, 1).steps == 4 && !loaded.at(2, 1).water(), "land stays land");
    expect(loaded.dirty_on_disk(), "a converted file is unsaved until written as v2");
    std::filesystem::remove_all(dir);
}

COOPA_TEST(water_floods_settles_and_drains) {
    using namespace toy::world;
    // A basin: ground 5 all round, a pit of 2 in the middle 3 x 3, one notch of 3 leading east.
    TileMapData d;
    d.resize(9, 9);
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 9; ++x) d.at(x, y).steps = (x >= 3 && x <= 5 && y >= 3 && y <= 5) ? 2 : 5;
    d.at(6, 4).steps = 3;
    const IRect flooded = flood_water(d, {{4, 4}}, 4);
    int wet = 0;
    for (const TileRecord& r : d.records()) wet += r.water() ? 1 : 0;
    expect(wet == 10 && d.at(6, 4).water() && d.at(6, 4).water_level() == 4, "a flood fills the basin and its notch, no further");
    expect(flooded.x0 == 3 && flooded.x1 == 7, "...and reports the rect it changed");
    // Lower a bank under the water: the water runs into it.
    d.at(7, 4).steps = 1;
    d.mark_dirty(IRect{7, 4, 8, 5});
    settle_water(d, IRect{7, 4, 8, 5});
    expect(d.at(7, 4).water() && d.at(7, 4).water_level() == 4, "ground dug beside water floods");
    // Raise a bed through its surface (as the brushes move ground: the water keeps its level).
    {
        TileRecord& bed = d.at(4, 4);
        const int level = bed.water_level();
        bed.steps = 6;
        bed.set_water(level);
    }
    settle_water(d, IRect{4, 4, 5, 5});
    expect(!d.at(4, 4).water(), "a bed raised through the water is dry land");
    expect(d.at(3, 4).water() && d.at(5, 4).water(), "...and the water around it stays");
    drain_water(d, {{3, 3}});
    wet = 0;
    for (const TileRecord& r : d.records()) wet += r.water() ? 1 : 0;
    expect(wet == 0, "drain takes the whole body");
}

/**
 * @brief A settle taken a few tiles at a time ends exactly where the all-at-once one does.
 *
 * This is the contract gameplay editing rests on: TerrainSystem spends a bounded number of
 * tiles a frame on a flood (TerrainComponent::water_settle_budget) so that digging beside the
 * sea cannot cost one frame the whole basin -- and the budget is allowed to change WHEN the
 * water arrives, never where it ends up.
 */
COOPA_TEST(a_budgeted_settle_reaches_the_same_water_as_one_call) {
    using namespace toy::world;
    // A long trench of ground 1 beside a lake at level 4: a flood with a lot of tiles to cross.
    const auto build = [] {
        TileMapData d;
        d.resize(40, 5);
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 40; ++x) d.at(x, y).steps = (y == 2) ? 1 : 6;
        d.at(0, 2).steps = 1;
        d.at(0, 2).set_water(4);
        return d;
    };

    TileMapData whole = build();
    settle_water(whole, IRect{0, 2, 1, 3});

    TileMapData stepped = build();
    WaterSettle settle = begin_settle(stepped, IRect{0, 2, 1, 3});
    int steps = 0;
    while (!settle.done() && steps < 1000) {
        step_settle(stepped, settle, 3);   // three tiles a "frame"
        ++steps;
    }
    expect(steps > 3, "settle: the trench took several steps to fill (" + std::to_string(steps) + ")");
    expect(settle.done(), "settle: ...and then finished");

    int same = 0, wet = 0;
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 40; ++x) {
            same += whole.at(x, y) == stepped.at(x, y) ? 1 : 0;
            wet += stepped.at(x, y).water() ? 1 : 0;
        }
    }
    expect(wet == 40, "settle: the water ran the length of the trench (" + std::to_string(wet) + " tiles)");
    expect(same == 200, "settle: a budgeted settle leaves exactly the tiles one call does");
}

/** @brief A tile flooded from a river is that river's: flowing, and carrying its direction. */
COOPA_TEST(water_running_in_keeps_the_body_it_came_from) {
    using namespace toy::world;
    TileMapData d;
    d.resize(6, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x) d.at(x, y).steps = 5;
    // A river tile at level 4, and a channel dug beside it.
    TileRecord& river = d.at(2, 1);
    river.steps = 3;
    river.set_water(4, false, true);
    river.flow = 3;
    d.at(3, 1).steps = 2;
    d.mark_dirty(IRect{3, 1, 4, 2});
    settle_water(d, IRect{3, 1, 4, 2});

    const TileRecord& filled = d.at(3, 1);
    expect(filled.water() && filled.water_level() == 4, "river: the dug tile fills to the river's level");
    expect(filled.has(TileRecord::Flowing) && filled.flow == 3, "river: ...as river water, flowing the same way");
    expect(!filled.has(TileRecord::Ocean), "river: ...and not as sea");
}

/**
 * @brief On a planet, water spreads onto the tile that is really beside it -- which across a
 *        cube face's edge is on ANOTHER face, turned (planet.h).
 *
 * Without the layout the walk steps one column in storage, which at a face edge is a column of
 * the next face nowhere near the water: the flood both misses the tile it should fill and fills
 * one on the far side of the globe.
 */
COOPA_TEST(water_settles_across_a_planets_faces) {
    using namespace toy::world;
    const int n = 8;
    PlanetLayout layout{n, 1.0f};
    TileMapData d;
    d.resize(6 * n, n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < 6 * n; ++x) d.at(x, y).steps = 5;

    // A sea tile on the +X face's last column, and the tile really beside it (next face).
    const glm::ivec2 edge{n - 1, 3};
    const glm::ivec2 across = layout.neighbour(edge.x, edge.y, 1, 0);
    expect(across.x / n != 0, "planet: the tile across the edge is on another face");
    d.at(edge.x, edge.y).steps = 2;
    d.at(edge.x, edge.y).set_water(4, true);
    d.at(across.x, across.y).steps = 1;          // dug, lower than the water beside it
    d.mark_dirty(IRect{across.x, across.y, across.x + 1, across.y + 1});

    settle_water(d, IRect{across.x, across.y, across.x + 1, across.y + 1}, IRect{}, k_water_flood_cap, &layout);
    const TileRecord& filled = d.at(across.x, across.y);
    expect(filled.water() && filled.water_level() == 4 && filled.has(TileRecord::Ocean),
           "planet: the sea runs across the face edge into the dug tile");

    // ...and the storage neighbour, which is NOT beside it on the globe, stays dry.
    const int storage_x = edge.x + 1;
    if (storage_x < 6 * n && !(storage_x == across.x && edge.y == across.y)) {
        expect(!d.at(storage_x, edge.y).water(),
               "planet: the merely storage-adjacent column of the next face is untouched");
    }
}

COOPA_TEST(generator_block_holds_only_what_differs) {
    using namespace toy::world;
    fkyaml::node comp = fkyaml::node::mapping();
    expect(generator_node(generator_config_from_node(comp)).get_value<fkyaml::node::mapping_type>().empty(),
           "no keys: mapcoopa's standard map, and nothing to write");
    const coopa::maps::MapConfig standard = coopa::maps::standard_config();
    expect(generator_config_from_node(comp).seed == standard.seed && generator_config_from_node(comp).grid_size == 80,
           "...which is the mapcoopa tool's own default world");

    comp["seed"] = fkyaml::node(9);
    comp["shape"] = fkyaml::node(std::string("circle"));
    comp["sea_level"] = fkyaml::node(0.3);
    comp["irregularity"] = fkyaml::node(0.4);
    coopa::maps::MapConfig legacy = generator_config_from_node(comp);
    expect(legacy.seed == 9 && legacy.shape.shape == coopa::maps::MapShape::Circle && legacy.sea_level == 0.3 &&
               legacy.shape.irregularity == 0.4,
           "older scenes' flat keys still load");

    const fkyaml::node block = generator_node(legacy);
    expect(block.contains("seed") && block.contains("sea_level") && block.contains("shape") && !block.contains("grid_size"),
           "the block holds the changed keys only");
    expect(block.at("shape").contains("shape") && !block.at("shape").contains("coast_detail"),
           "...nested blocks too");
    fkyaml::node again = fkyaml::node::mapping();
    again["generator"] = block;
    const coopa::maps::MapConfig back = generator_config_from_node(again);
    expect(fkyaml::node::serialize(coopa::maps::config_to_node(back)) == fkyaml::node::serialize(coopa::maps::config_to_node(legacy)),
           "the block round-trips to the same settings");
}
