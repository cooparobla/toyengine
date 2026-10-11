/**
 * @file tile_mode_test.cpp
 * @brief Tile Mode through real input: a TileTerrain is added and baked, Tile Mode entered, a
 *        Raise stroke dragged across it lifts the tiles under it by whole steps as one undo step
 *        (in the scene's timeline), undo / redo restore exactly, a rectangle selection's tiles are
 *        edited from the panel, and saving the scene writes the terrain's .tiles file.
 */

#include <coopa/testing/test.h>

#include <filesystem>

#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_map_data.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("tile_mode");

namespace toy::editor::testing {

COOPA_TEST(tile_mode_raises_selects_undoes_and_saves) {
    using coopa::input::MouseButton;
    toy::world::TileMapStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    // A small world, so the bake takes a moment: 32 cells x 2 tiles.
    const ObjectId id = app.create_with_component("TileTerrain", "Tile Terrain");
    {
        auto& doc = app.document();
        const int ci = doc.find_component(id, "TileTerrain");
        Node comp = doc.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
        comp["grid_size"] = Node(static_cast<int64_t>(32));
        comp["tiles_per_grid_unit"] = Node(static_cast<int64_t>(2));
        comp["chunk_size"] = Node(static_cast<int64_t>(16));
        comp["view_radius"] = Node(static_cast<int64_t>(2));
        doc.set_component(id, ci, comp, "Small world");
        app.sync().rebuild(engine, doc);
    }
    toy::world::TerrainComponent* t = nullptr;
    for (int i = 0; i < 900; ++i) {
        tick(engine, 1);
        coopa::scene::SceneObject* live = app.sync().live(id);
        t = live ? live->get_component<toy::world::TerrainComponent>() : nullptr;
        if (t && t->is_ready() && !t->chunks().empty()) break;
    }
    coopa::test::require(t != nullptr && t->tiles && !t->tiles->empty(), "the tile terrain bakes its world");
    const std::shared_ptr<toy::world::TileMapData> tiles = t->tiles;
    expect(tiles->width() == 64 && tiles->dirty_on_disk(), "a fresh bake is 64 x 64 tiles, unsaved");

    app.document().select(id);
    app.set_tile_mode(true);
    tick(engine, 2);
    expect(app.tile_mode(), "Tile Mode entered");

    // Look down at a land tile from above: find one, then frame the camera on it.
    glm::ivec2 land{-1};
    for (int y = 16; y < 48 && land.x < 0; ++y)
        for (int x = 16; x < 48; ++x)
            if (!tiles->at(x, y).water()) { land = {x, y}; break; }
    coopa::test::require(land.x >= 0, "the world has land in its middle");
    app.frame_selected();
    tick(engine, 30);

    auto& brush = app.tile_brush();
    brush.tool = TileTool::Raise;
    brush.size = 3;
    brush.strength = 2;
    const float ts = t->params.tile_size, hs = t->params.height_step;
    auto at_tile = [&](int x, int y) {
        return screen_of(engine, app, id, glm::vec3((x + 0.5f) * ts, (y + 0.5f) * ts, tiles->at(x, y).steps * hs), in.scale);
    };
    const auto a = at_tile(land.x, land.y);
    coopa::test::require(a.has_value(), "the tile is on screen");

    const std::vector<toy::world::TileRecord> before = tiles->records();
    const std::size_t undo0 = app.tile_history().undo_count();
    in.move(*a);
    in.drag(*a + glm::vec2(1.0f, 0.0f), MouseButton::Left, 4);
    tick(engine, 3);
    expect(app.tile_history().undo_count() == undo0 + 1, "the stroke is one undo step");
    int raised = 0, wrong = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const int dz = tiles->records()[i].steps - before[i].steps;
        if (dz == 2) ++raised;
        else if (dz != 0 && !before[i].water()) ++wrong;
    }
    expect(raised >= 1 && raised <= 9 && wrong == 0,
           "Raise lifts the brush's tiles by exactly two steps (" + std::to_string(raised) + " raised)");
    dump(engine, "tile_mode_raise");

    app.undo();
    tick(engine, 2);
    expect(tiles->records() == before, "undo restores the exact tiles");
    app.redo();
    tick(engine, 2);
    expect(tiles->records() != before, "redo re-applies the stroke");

    // Select a rectangle by dragging with the Select tool, then edit it as the panel does.
    brush.tool = TileTool::Select;
    const auto b = at_tile(std::min(land.x + 3, 63), std::min(land.y + 2, 63));
    coopa::test::require(b.has_value(), "the selection's far corner is on screen");
    in.move(*a);
    in.drag(*b, MouseButton::Left, 8);
    tick(engine, 2);
    const int selected = app.tile_selection().count();
    expect(selected >= 4, "dragging with Select selects a rectangle (" + std::to_string(selected) + " tiles)");
    dump(engine, "tile_mode_selection");
    app.tile_edit_selection("Set Tile Biome", [](toy::world::TileRecord& r) { r.biome = 5; });
    int edited = 0;
    app.tile_selection().for_each([&](int x, int y) { edited += tiles->at(x, y).biome == 5; });
    expect(edited == selected, "a selection edit reaches every selected tile");
    expect(app.tile_history().undo_count() == undo0 + 2, "...as one more undo step");

    const auto scene_path = session.project.assets() / "scenes" / "tile_test" / "scene.yaml";
    std::filesystem::create_directories(scene_path.parent_path());
    expect(app.save_scene_as(scene_path), "the scene saves");
    const std::string file = toy::world::TileMapStore::instance().path_of(*tiles);
    expect(!file.empty() && std::filesystem::exists(file), "saving writes the .tiles file (" + file + ")");
    expect(!tiles->dirty_on_disk(), "the tiles are clean after saving");
    toy::world::TileMapData reread;
    expect(reread.load(file) && reread.records() == tiles->records(), "the file holds exactly the edited tiles");

    app.set_tile_mode(false);
    tick(engine, 2);
    expect(!app.tile_mode(), "Tile Mode exited");
    toy::world::TileMapStore::instance().clear();
}

} // namespace toy::editor::testing
