/**
 * @file world_generator_test.cpp
 * @brief The World Generator window: it opens on a TileTerrain with the terrain's settings (old
 *        flat keys included), previews the map, and Generate World writes the `generator:` block
 *        and re-bakes the tiles -- two undo steps in the scene's timeline, which restore the
 *        tiles and the settings exactly.
 */

#include <coopa/testing/test.h>

#include <toyengine/mapgen/map_generator_component.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_map_data.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("world_generator");

namespace toy::editor::testing {

COOPA_TEST(world_generator_previews_generates_and_undoes) {
    toy::world::TileMapStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;

    const ObjectId id = app.create_with_component("TileTerrain", "Tile Terrain");
    auto component = [&] {
        const int ci = app.document().find_component(id, "TileTerrain");
        return Node(app.document().find(id)->at("components").as_seq()[static_cast<std::size_t>(ci)]);
    };
    {
        // A small world, written the old way (flat keys), as an older scene would have it.
        Node comp = component();
        comp["grid_size"] = Node(static_cast<int64_t>(24));
        comp["tiles_per_grid_unit"] = Node(static_cast<int64_t>(2));
        comp["chunk_size"] = Node(static_cast<int64_t>(16));
        app.document().set_component(id, app.document().find_component(id, "TileTerrain"), comp, "Small world");
        app.sync().rebuild(engine, app.document());
    }
    // The world's settings live on a MapGenerator component beside the terrain (toyengine/mapgen):
    // generation is not the terrain's, so applying writes them there. A scene authored the old
    // way has them on the terrain, and applying migrates them across -- which is what the
    // assertions below check, since that migration is the behaviour users rely on.
    auto generator_component = [&] {
        const int gi = app.document().find_component(id, "MapGenerator");
        return gi < 0 ? Node() : Node(app.document().find(id)->at("components").as_seq()[static_cast<std::size_t>(gi)]);
    };
    toy::world::TerrainComponent* t = nullptr;
    auto live = [&] {
        coopa::scene::SceneObject* o = app.sync().live(id);
        return o ? o->get_component<toy::world::TerrainComponent>() : nullptr;
    };
    auto live_generator = [&] {
        coopa::scene::SceneObject* o = app.sync().live(id);
        return o ? o->get_component<toy::mapgen::MapGeneratorComponent>() : nullptr;
    };
    for (int i = 0; i < 900; ++i) {
        tick(engine, 1);
        t = live();
        if (t && t->is_ready() && !t->baking()) break;
    }
    coopa::test::require(t != nullptr && t->tiles && t->tiles->width() == 48, "the terrain bakes its 24-cell world");
    const std::shared_ptr<toy::world::TileMapData> tiles = t->tiles;
    const std::vector<toy::world::TileRecord> before = tiles->records();

    app.open_world_generator(id);
    tick(engine, 3);
    expect(app.world_generator_open(), "the World Generator opens on the terrain");
    expect(app.world_generator_config().grid_size == 24, "...with its settings, old flat keys included");
    for (int i = 0; i < 600 && app.world_generator_preview_serial() == 0; ++i) tick(engine, 1);
    expect(app.world_generator_preview_serial() > 0, "a preview of the map lands");

    const std::uint64_t previews = app.world_generator_preview_serial();
    app.world_generator_config().seed = 77;
    app.world_generator_config().sea_level = 0.35;
    expect(app.world_generator_world_tiles() == 48, "the world size starts as the terrain's (24 cells x 2 tiles)");
    app.world_generator_world_tiles() = 60;   // any size, whatever the cell count
    for (int i = 0; i < 600 && app.world_generator_preview_serial() == previews; ++i) tick(engine, 1);
    expect(app.world_generator_preview_serial() > previews, "editing a setting previews again");
    dump(engine, "world_generator");

    const std::uint64_t bakes = tiles->bakes_done();
    const std::size_t undo0 = app.tile_history().undo_count();
    app.world_generator_generate();
    for (int i = 0; i < 900; ++i) {
        tick(engine, 1);
        if (tiles->bakes_done() > bakes && app.tile_history().undo_count() > undo0) break;
    }
    expect(!app.world_generator_open(), "Generate World closes the window");
    const Node comp = component();
    const Node gen = generator_component();
    expect(gen.contains("generator") && get_int(gen["generator"], "seed", 0) == 77,
           "the settings are written as the MapGenerator's generator block");
    expect(!gen.contains("seed") && get_int(gen["generator"], "grid_size", 0) == 24,
           "...and the old flat keys move into it");
    expect(!comp.contains("generator") && !comp.contains("seed") && !comp.contains("grid_size"),
           "...and off the terrain, which no longer generates anything");
    toy::mapgen::MapGeneratorComponent* g = live_generator();
    expect(g != nullptr && g->config.seed == 77 && g->config.sea_level == 0.35,
           "the live generator has them");
    expect(tiles->bakes_done() > bakes && tiles->records() != before, "the tiles are re-baked from them");
    expect(get_int(gen, "world_tiles", 0) == 60 && tiles->width() == 60 && tiles->height() == 60,
           "...at the chosen world size: 60 x 60 tiles from 24 map cells (" + std::to_string(tiles->width()) + ")");

    app.undo();
    tick(engine, 3);
    expect(tiles->records() == before && tiles->width() == 48, "undo restores the tiles, size and all");
    app.undo();
    tick(engine, 3);
    const Node restored = component();
    expect(!restored.contains("generator") && get_int(restored, "grid_size", 0) == 24,
           "a second undo restores the settings to the terrain, where this scene authored them");
    expect(app.document().find_component(id, "MapGenerator") < 0,
           "...and takes the generator component with them: the apply is ONE undo step, so Ctrl Z "
           "cannot leave the scene half-migrated");
    toy::world::TileMapStore::instance().clear();
}

} // namespace toy::editor::testing
