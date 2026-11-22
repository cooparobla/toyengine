/**
 * @file editor_sub_scene_test.cpp
 * @brief A scene that declares a SubScene, opened in the editor and taken through play/stop.
 *
 * The editor is where a sub-view's lifetime is hardest: entering play mode PUSHES a second copy
 * of the scene (so a second view is declared while the first still exists), leaving it REMOVES
 * that scene outright, and `ensure_active_scene_()` forces scene exclusivity on both
 * transitions. Each of those is a chance to leave a view pointing at a scene that is gone, or to
 * construct a pipeline per frame without ever freeing one.
 *
 * Repeating the cycle is the point: a single play/stop can hide a leak that three make obvious.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <string>

#include <coopa/yaml/document.h>
#include <toyengine/core/engine.h>
#include <toyengine/render/sub_scene.h>

#include "editor/support/editor_session.h"

namespace toy::editor::testing {

namespace fs = std::filesystem;

COOPA_TEST_SUITE("editor_sub_scene");

namespace {

/// Copies the hosting scene and the scene it renders into the fresh project.
void copy_demo(Project& project) {
    const fs::path scenes = project.assets() / "scenes" / "ui";
    fs::create_directories(scenes);
    for (const char* name : {"glass_ui_demo", "map_world_demo"}) {
        fs::copy(fs::path(ROOT_DIR) / "assets" / "scenes" / "ui" / name, scenes / name,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    }
}

}  // namespace

/**
 * @brief Play and stop, repeatedly, without losing or multiplying sub-views.
 *
 * The scene count is the assertion that catches both failure modes at once: a view left behind
 * when its scene went adds scenes, and a view never created removes them. It must come back to
 * the same number every time round.
 */
COOPA_TEST(play_and_stop_repeatedly_keeps_one_sub_view) {
    EditorSession session({.prepare = copy_demo});
    auto& engine = session.engine;
    auto& app = session.app;

    const fs::path scene = session.project.assets() / "scenes" / "ui" / "glass_ui_demo" / "scene.yaml";
    coopa::test::require(app.open_scene(scene), "the hosting scene opens in the editor");
    tick(engine, 8);

    // Edit mode: the edit scene declares the view, so its sub-scene is loaded beside it.
    const size_t edit_scenes = engine.scene_manager().scenes().size();
    expect(edit_scenes >= 2, "the edit scene brought its sub-scene with it (" +
                                 std::to_string(edit_scenes) + " scenes)");

    for (int round = 0; round < 3; ++round) {
        app.play();
        tick(engine, 10);
        app.stop();
        tick(engine, 10);

        const size_t now = engine.scene_manager().scenes().size();
        expect(now == edit_scenes,
               "round " + std::to_string(round) + ": back to the same scenes after stop (" +
                   std::to_string(now) + ", expected " + std::to_string(edit_scenes) + ")");
    }

    // And the view is still live afterwards, rather than merely balanced in count.
    auto views = engine.scene().get_components<toy::render::SubScene>();
    coopa::test::require(views.size() == 1, "the edit scene still declares exactly one SubScene");
    expect(views[0]->ready && views[0]->texture.valid(),
           "it is still publishing a texture after three play/stop cycles");
}

/**
 * @brief Hovering the view hands its scene input, and handing it back does not stick.
 *
 * The routing path -- pointer_owner_(), the cursor remap into the view's own pixels, and
 * driving a SECOND scene's input in the same frame -- runs only when there is a real cursor
 * over a drawn view, so NO_INPUT tests never reach it. Exercised here with the pointer moving
 * in and out of the panel across play/stop, which is the shape of actually using the thing.
 */
COOPA_TEST(hovering_the_view_routes_input_without_sticking) {
    EditorSession session({.fixed_dt = "0.016666", .prepare = [](Project& project) {
        copy_demo(project);
        // Open the dock on the World group so the Map panel -- and therefore the view -- is the
        // one on screen and can be hovered.
        const fs::path scene = project.assets() / "scenes" / "ui" / "glass_ui_demo" / "scene.yaml";
        fkyaml::node doc = coopa::yaml::load_document(scene);
        for (auto& obj : doc["scene"]["root_objects"].as_seq()) {
            if (!obj.contains("name") || obj["name"].get_value<std::string>() != "dock") continue;
            for (auto& c : obj["components"].as_seq()) {
                if (!c.contains("type") || c["type"].get_value<std::string>() != "QuadrantDock") continue;
                c["open_inventory"] = false;
                c["open_character"] = false;
                c["open_skills"]    = false;
                c["reduced_motion"] = true;
            }
        }
        coopa::yaml::save_document(scene, doc);
    }});
    auto& engine = session.engine;
    auto& app = session.app;

    const fs::path scene = session.project.assets() / "scenes" / "ui" / "glass_ui_demo" / "scene.yaml";
    coopa::test::require(app.open_scene(scene), "the hosting scene opens");
    tick(engine, 8);

    app.play();
    tick(engine, 12);

    // Sweep the pointer across the viewport, in and out of wherever the panel landed, including
    // a drag -- a hand-over mid-drag is what would otherwise leave a button stuck down.
    for (int i = 0; i < 12; ++i) {
        const float t = static_cast<float>(i) / 11.0f;
        session.in.move({120.0f + t * 900.0f, 180.0f + t * 600.0f}, 2);
    }
    session.in.drag({400.0f, 500.0f}, coopa::input::MouseButton::Left, 6);
    tick(engine, 8);
    session.in.move({1700.0f, 950.0f}, 4);   // well clear of any panel
    tick(engine, 8);

    app.stop();
    tick(engine, 12);

    expect(engine.has_scene(), "the engine still has a scene after hovering and stopping");
    auto views = engine.scene().get_components<toy::render::SubScene>();
    coopa::test::require(views.size() == 1, "the edit scene still declares one SubScene");
    expect(views[0]->ready, "its view is still live after the pointer passed over it");
}

} // namespace toy::editor::testing
