/**
 * @file terrain_sculpt_test.cpp
 * @brief Terrain Mode through real input: a HeightmapTerrain is added, Terrain Mode entered, a
 *        Raise stroke dragged across it lifts the ground as one undo step (in the scene's
 *        timeline), undo restores the exact grid, a Paint stroke paints its layer, and saving the
 *        scene writes the terrain's heightmap and splat PNGs.
 */

#include <coopa/testing/test.h>

#include <filesystem>

#include <toyengine/world/heightmap_store.h>
#include <toyengine/world/heightmap_terrain.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("terrain_sculpt");

namespace toy::editor::testing {

COOPA_TEST(terrain_mode_sculpts_paints_undoes_and_saves) {
    using coopa::input::MouseButton;
    toy::world::HeightmapStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const ObjectId id = app.create_with_component("HeightmapTerrain", "Terrain");
    tick(engine, 6);
    expect(id != 0, "Add > Heightmap Terrain creates a terrain object");
    app.document().select(id);
    app.frame_selected();
    tick(engine, 30);
    auto* t = engine.scene().find_first_component<toy::world::HeightmapTerrain>();
    expect(t != nullptr && t->data != nullptr, "the terrain is live in the editor's scene");
    if (t == nullptr || t->data == nullptr) return;
    // The data outlives the component: the editor rebuilds the object (and its component) on
    // every document change, but the data is shared through the HeightmapStore.
    const std::shared_ptr<toy::world::HeightmapData> data = t->data;
    const glm::vec2 size = data->geometry().size;
    const float h = data->sample_height(size * 0.5f);

    app.set_terrain_mode(true);
    tick(engine, 2);
    expect(app.terrain_mode(), "Terrain Mode entered");
    auto& brush = app.terrain_brush();
    brush.tool = TerrainTool::Raise;
    brush.radius = 20.0f;
    brush.strength = 1.0f;

    // A stroke across the middle of the terrain (object-local metres).
    auto a = screen_of(engine, app, id, glm::vec3(size.x * 0.4f, size.y * 0.5f, h), in.scale);
    auto b = screen_of(engine, app, id, glm::vec3(size.x * 0.6f, size.y * 0.5f, h), in.scale);
    expect(a && b, "the stroke lies on screen");
    if (!a || !b) return;
    const std::vector<float> before = data->heights();
    const std::size_t undo0 = app.terrain_history().undo_count();
    in.move(*a);
    in.drag(*b, MouseButton::Left, 12);
    tick(engine, 3);
    expect(app.terrain_history().undo_count() == undo0 + 1, "the stroke is one undo step");
    float lifted = 0.0f;
    for (std::size_t i = 0; i < before.size(); ++i) lifted = std::max(lifted, data->heights()[i] - before[i]);
    expect(lifted > 0.5f, "Raise lifts the ground under the stroke (" + std::to_string(lifted) + " m)");
    dump(engine, "terrain_raise_stroke");

    app.undo();
    tick(engine, 2);
    expect(data->heights() == before, "undo restores the exact grid");
    app.redo();
    tick(engine, 2);
    expect(data->heights() != before, "redo re-applies the stroke");

    // Paint layer 0 is the only layer: paint and erase are both well-defined; paint must keep it.
    brush.tool = TerrainTool::Paint;
    brush.layer = 0;
    in.move(*a);
    in.drag(*b, MouseButton::Left, 8);
    tick(engine, 2);
    expect(app.terrain_history().undo_count() == undo0 + 2, "the paint stroke is one more undo step");

    expect(data->dirty_on_disk(), "the edits are unsaved");
    const auto scene_path = session.project.assets() / "scenes" / "terrain_test" / "scene.yaml";
    std::filesystem::create_directories(scene_path.parent_path());
    expect(app.save_scene_as(scene_path), "the scene saves");
    const toy::world::HeightmapFiles files = toy::world::HeightmapStore::instance().files_of(data.get());
    expect(!files.height.empty() && std::filesystem::exists(files.height), "saving writes the heightmap PNG");
    expect(std::filesystem::exists(files.splat0), "...and the splat PNGs");
    expect(!data->dirty_on_disk(), "the terrain is clean after saving");

    app.set_terrain_mode(false);
    tick(engine, 2);
    expect(!app.terrain_mode(), "Terrain Mode exited");
    toy::world::HeightmapStore::instance().clear();
}

} // namespace toy::editor::testing
