/**
 * @file world_generator_test.cpp
 * @brief The World Generator window: it opens on a TileTerrain with the terrain's settings (old
 *        flat keys included), previews the map, and Generate World writes the `generator:` block
 *        and re-bakes the tiles -- two undo steps in the scene's timeline, which restore the
 *        tiles and the settings exactly.
 */

#include <coopa/testing/test.h>

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
    toy::world::TerrainComponent* t = nullptr;
    auto live = [&] {
        coopa::scene::SceneObject* o = app.sync().live(id);
        return o ? o->get_component<toy::world::TerrainComponent>() : nullptr;
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
    expect(comp.contains("generator") && get_int(comp["generator"], "seed", 0) == 77,
           "the settings are written as the generator block");
    expect(!comp.contains("seed") && !comp.contains("grid_size") && get_int(comp["generator"], "grid_size", 0) == 24,
           "...and the old flat keys move into it");
    t = live();
    expect(t != nullptr && t->generator.seed == 77 && t->generator.sea_level == 0.35, "the live terrain has them");
    expect(tiles->bakes_done() > bakes && tiles->records() != before, "the tiles are re-baked from them");

    app.undo();
    tick(engine, 3);
    expect(tiles->records() == before, "undo restores the tiles");
    app.undo();
    tick(engine, 3);
    const Node restored = component();
    expect(!restored.contains("generator") && get_int(restored, "grid_size", 0) == 24, "a second undo restores the settings");
    toy::world::TileMapStore::instance().clear();
}

} // namespace toy::editor::testing
