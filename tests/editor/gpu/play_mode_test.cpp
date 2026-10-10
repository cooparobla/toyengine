/**
 * @file play_mode_test.cpp
 * @brief Play mode inside the editor: the game gets input only once its viewer is clicked (Esc hands it
 * back while play continues), Pause freezes and Step advances exactly one frame, and the
 * selection outline belongs to the editor, not the game.
 */

#include <coopa/testing/test.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("play_mode");

namespace toy::editor::testing {

/**
 * @brief Play mode shows the game, and the game gets input only when asked: it starts without
 *        the mouse and keyboard until its viewer is clicked, Esc hands them back while play
 *        continues, F5 stops. A selection made before Play draws no outline in the game, and is
 *        kept (and outlined again) after Stop.
 */
COOPA_TEST(play_takes_input_on_click_and_hides_the_selection_outline) {
    using coopa::input::Key;
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    ObjectId mesh = 0;
    for (const auto& [id, live] : app.sync().live_objects()) {
        if (live && live->get_component<coopa::gfx::engine::components::MeshRenderer>()) { mesh = id; break; }
    }
    require(mesh != 0, "the default project has a mesh object");
    app.document().select(mesh, false);
    tick(engine, 2);
    expect(app.selection_outlines_drawn() == 1, "the selected object is outlined in the editor");

    app.play();
    tick(engine, 3);
    expect(app.playing(), "playing");
    expect(app.selection_outlines_drawn() == 0, "no selection outline while playing");
    expect(!app.game_focused() && !engine.game_input_focus(), "the game starts without the mouse and keyboard");

    in.click(app.viewport_box().center());
    expect(app.game_focused() && engine.game_input_focus(), "clicking the viewer gives the game input");
    // While the game has the mouse, the editor UI ignores clicks (File menu stays shut).
    in.click({44, 14});
    expect(!app.ui().any_popup_open(), "editor menus ignore clicks while the game has the mouse");

    in.key(Key::Escape);
    expect(!app.game_focused() && !engine.game_input_focus(), "Esc hands the mouse back");
    expect(app.playing() && engine.scene().is_simulating(), "and the game keeps running");

    in.click(app.viewport_box().center());
    expect(app.game_focused(), "clicking again refocuses");
    in.key(Key::F5);
    tick(engine, 2);
    expect(!app.playing() && !app.game_focused() && engine.game_input_focus(), "F5 stops and restores the engine default");
    tick(engine, 2);
    expect(app.document().is_selected(mesh) && app.selection_outlines_drawn() == 1,
           "the selection is kept and outlined again after Stop");
}

/** @brief Pause freezes the simulation and Step advances exactly one frame; also the Blender
 *         chrome around it: nav-gizmo axis clicks (never selecting what is behind), Properties
 *         tabs per selection, and the outliner eye (editor-only hiding). */
COOPA_TEST(pause_step_nav_gizmo_property_tabs_and_outliner_eye) {
    using coopa::input::Key;
    using coopa::input::Mods;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    // Navigation gizmo: click the ball for +Z (screen position from the live view).
    const imm::Box g = app.nav_gizmo_rect();
    auto ball = [&](glm::vec3 axis) {
        const glm::mat3 vr(coopa::gfx::engine::components::CameraComponent::main()->get_view_matrix());
        const glm::vec3 v = vr * axis;
        return glm::vec2(g.x + 55, g.y + 55) + glm::vec2(v.x, -v.y) * 40.0f;
    };
    in.click(ball(glm::vec3(0, 0, 1)));
    expect(std::abs(app.camera().pitch_deg - 89.5f) < 0.01f, "clicking the gizmo's Z ball gives the top view");
    in.click(ball(glm::vec3(0, -1, 0)));
    expect(std::abs(app.camera().pitch_deg) < 0.01f && std::abs(app.camera().yaw_deg) < 0.01f,
           "clicking its -Y ball gives the front view (pitch " + std::to_string(app.camera().pitch_deg) + ")");
    expect(app.document().selection().empty(), "gizmo clicks never select objects behind it");

    // Properties tabs follow the selection (Blender's object tabs appear with an object).
    app.document().clear_selection();
    app.set_prop_tab(PropTab::Material);
    tick(engine, 2);
    expect(app.prop_tab() == PropTab::Tool, "without a selection, object tabs fall back to Tool");
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") app.document().select(id);
    app.set_prop_tab(PropTab::Data);
    tick(engine, 2);
    expect(app.prop_tab() == PropTab::Data, "a mesh object offers the Object Data tab");

    // Outliner eye: hides the object in the viewport (editor-only, the document is untouched).
    {
        const ObjectId sel = app.document().primary();
        expect(app.outliner_eye_rect(sel).has_value(), "the selected object's outliner row has an eye toggle");
        if (auto eye = app.outliner_eye_rect(sel)) {
            in.click(glm::vec2(eye->x + eye->w * 0.5f, eye->y + eye->h * 0.5f));
            tick(engine, 2);
            auto* live = app.sync().live(sel);
            expect(live && !live->active(), "clicking the eye hides the object in the viewport");
            expect(get_bool(*app.document().find(sel), "active", true), "and leaves the saved `active` flag alone");
            in.click(glm::vec2(eye->x + eye->w * 0.5f, eye->y + eye->h * 0.5f));
            tick(engine, 2);
            expect(live && live->active(), "clicking it again reveals it");
        }
    }

    // Unity play controls: Pause freezes, Step advances exactly one frame.
    ObjectId cube = 0;
    for (ObjectId id : app.document().all_ids()) if (get_string(*app.document().find(id), "name") == "cube") cube = id;
    expect(app.document().selection().empty(), "hiding deselects, as in Blender");
    app.document().select(cube);
    Node rb = default_component("Rigidbody");
    app.document().add_component(cube, rb);
    Node box = default_component("BoxCollider");   // physics bodies are built from colliders
    app.document().add_component(cube, box);
    app.document().set_transform(cube, {0, 0, 5}, glm::vec3(0), glm::vec3(1), "Lift");
    app.sync().rebuild(engine, app.document());
    tick(engine, 2);
    app.play();
    tick(engine, 5);
    expect(app.playing(), "playing");
    app.pause();
    tick(engine, 1);
    expect(app.paused(), "Pause freezes the simulation");
    auto z_of = [&] { return engine.scene().find_object("cube")->get_transform()->transform().position().z; };
    const float z0 = z_of();
    tick(engine, 5);
    expect(std::abs(z_of() - z0) < 1e-6f, "nothing moves while paused");
    app.step();
    tick(engine, 4);
    const float z1 = z_of();
    expect(z1 < z0 - 1e-6f && app.paused(), "Step advances one frame and stays paused (" + std::to_string(z0) + " -> " + std::to_string(z1) + ")");
    tick(engine, 4);
    expect(std::abs(z_of() - z1) < 1e-6f, "and then holds still");
    app.stop();
    tick(engine, 2);
    expect(!app.playing(), "stop");
    app.open_asset(AssetType::Material, "materials/default.yaml");
    tick(engine, 4);
    dump(engine, "07_shading_workspace");
}

} // namespace toy::editor::testing
