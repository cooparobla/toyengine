/**
 * @file play_mode_test.cpp
 * @brief Play mode inside the editor: the game gets input only once its viewer is clicked (Esc hands it
 * back while play continues), Pause freezes and Step advances exactly one frame, and the
 * selection outline belongs to the editor, not the game.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

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
    coopa::test::require(mesh != 0, "the default project has a mesh object");
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
    tick(engine, 1);   // flushes play()'s deferred build (editor_rebuild_splits_the_frame)
    coopa::test::require(app.playing(), "playing");
    // Pause/Step refuse while the Play loading cover is still up (editor_app.h:279's loading()),
    // so run out the cover's warm-up + fade before exercising them.
    for (int i = 0; i < 60 && engine.is_loading(); ++i) tick(engine, 1);
    expect(!app.loading(), "the loading cover has cleared");
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

/**
 * @brief Play's loading cover holds fully opaque through Warming. begin_scene_warmup() starts
 *        the state with alpha already at 1 (the scene is already built -- there is nothing left
 *        to cover up for) but elapsed at 0; the fade math must not mistake that for "just
 *        starting to cover" and drag alpha back down toward 0 on the very next frame.
 */
COOPA_TEST(play_cover_alpha_does_not_collapse_on_the_first_warming_frame) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;

    app.play();
    tick(engine, 1);
    coopa::test::require(app.playing(), "playing");
    auto h = engine.scene_load();
    coopa::test::require(h.valid(), "Play starts a loading cover");
    expect(h.transition_alpha() > 0.99f,
           "alpha stays at 1 on the first Warming frame (" + std::to_string(h.transition_alpha()) + ")");

    tick(engine, 1);
    h = engine.scene_load();
    expect(!h.valid() || h.transition_alpha() > 0.99f,
           "and the next frame (" + (h.valid() ? std::to_string(h.transition_alpha()) : std::string("n/a")) + ")");

    for (int i = 0; i < 60 && engine.is_loading(); ++i) tick(engine, 1);
    expect(!engine.is_loading(), "the cover clears on its own once warm-up finishes");
    app.stop();
    tick(engine, 2);
}

/**
 * @brief Play builds its scene from the document inside the frame's post-late-update window --
 *        after the update that would resolve its transforms, before that same frame's render --
 *        so the engine resolves the tree itself ahead of the render (Engine::tick()). Without
 *        that, the render gather read a world matrix no resolve pass had computed: every object
 *        of the scene, on the first frame of every Play (an assert in a debug build, a stale
 *        matrix in a release one).
 */
COOPA_TEST(play_resolves_the_world_matrices_of_the_scene_it_just_built) {
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;

    app.play();
    tick(engine, 1);   // the frame that both builds the play scene and draws it
    coopa::test::require(app.playing(), "playing");

    int transforms = 0, dirty = 0;
    for (auto& root : engine.scene().root_objects()) {
        root->for_each_recursive([&](coopa::scene::SceneObject& o) {
            if (auto* tc = o.get_transform()) {
                ++transforms;
                if (tc->transform().is_dirty()) ++dirty;
            }
        });
    }
    expect(transforms > 0, "the play scene has transforms to resolve");
    expect(dirty == 0, "every one of them is resolved on the first drawn frame of Play (" +
                           std::to_string(dirty) + " of " + std::to_string(transforms) + " dirty)");
    app.stop();
    tick(engine, 2);
}

/**
 * @brief Regression for "no loading screen in the editor": Play's cover must draw the built-in
 *        cycling mark over the viewport, not just a blank fade. begin_scene_warmup() starts the
 *        Warming stage already covered (alpha 1 -- see the alpha-collapse test above), but the
 *        viewport's render pipeline takes a couple of frames to pick up the new overlay layer,
 *        so this waits past that before capturing -- the earlier alpha-only test cannot catch
 *        a render-side regression like this one, only a logic-side one.
 */
COOPA_TEST(play_cover_draws_the_mark_in_the_viewport) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;

    app.play();
    tick(engine, 3);
    coopa::test::require(app.playing(), "playing");
    auto h = engine.scene_load();
    coopa::test::require(h.valid(), "Play starts a loading cover");
    coopa::test::require(h.transition_alpha() > 0.99f, "caught the cover fully opaque");

    const coopa::gfx::util::ImageData img = engine.capture_image(false);
    const auto d = engine.display_rect();
    auto pixel_sum = [&](float fx, float fy) {
        const uint32_t x = static_cast<uint32_t>(d.x) + static_cast<uint32_t>(fx * d.w);
        const uint32_t y = static_cast<uint32_t>(d.y) + static_cast<uint32_t>(fy * d.h);
        const size_t i = (static_cast<size_t>(y) * img.width + x) * img.channels;
        return int(img.pixels[i]) + int(img.pixels[i + 1]) + int(img.pixels[i + 2]);
    };
    // The default loading.background is pure black; count_bright finds the mark's corner.
    long long bright = 0;
    const uint32_t xa = static_cast<uint32_t>(d.x) + static_cast<uint32_t>(0.70f * d.w);
    const uint32_t xb = static_cast<uint32_t>(d.x) + d.w;
    const uint32_t ya = static_cast<uint32_t>(d.y) + static_cast<uint32_t>(0.70f * d.h);
    const uint32_t yb = static_cast<uint32_t>(d.y) + d.h;
    for (uint32_t y = ya; y < yb && y < img.height; ++y) {
        for (uint32_t x = xa; x < xb && x < img.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * img.width + x) * img.channels;
            if (int(img.pixels[i]) + int(img.pixels[i + 1]) + int(img.pixels[i + 2]) > 60) ++bright;
        }
    }
    expect(bright > 50, "play: the loading mark is drawn in the viewport's bottom-right corner (" +
                             std::to_string(bright) + " px)");
    // The centre, not a corner: the viewport's own toolbar, nav gizmo and PLAYING badge all sit
    // in corners (viewport_chrome.cpp), so a corner sample can catch editor chrome rather than
    // the cover underneath it.
    expect(pixel_sum(0.5f, 0.5f) < 20, "play: away from the mark the cover stays its own background");
    dump(engine, "12_play_loading_cover");

    for (int i = 0; i < 60 && engine.is_loading(); ++i) tick(engine, 1);
    app.stop();
    tick(engine, 2);
}

} // namespace toy::editor::testing
