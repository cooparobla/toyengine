/**
 * @file ui_render_test.cpp
 * @brief UI reaching the screen: a world-space canvas button lights up under the pointer (window
 *        pixel -> letterbox -> NDC -> ray -> canvas -> EventSystem -> Button), and the ui_showcase
 *        HUD prefab expands, binds its Health bar by name and draws over the frame.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <uicoopa/ui_yaml.h>
#include <toyengine/render/pixel_math.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("ui_render");

using namespace toy::test;

/**
 * @brief Parks the pointer on the world-space "Heal" Button in world_canvas_test, then moves it
 * off, and checks the button lights up and goes out -- the end-to-end proof that world-space UI
 * is both DRAWN and INTERACTIVE.
 *
 * Everything between a window pixel and a tinted button is exercised here and nowhere else:
 * window pixel -> letterbox rect -> internal render extent -> NDC (through the negative-height
 * viewport convention) -> world ray -> ray/plane intersection against the canvas -> canvas
 * pixels -> Raycaster -> EventSystem -> Button::on_pointer_enter -> ColorTransition. uicoopa's
 * own headless tests cover the ray/plane maths exactly; only a real render can cover the rest
 * of that chain, because the letterbox and viewport conventions live in this repo.
 *
 * It also answers the "does world UI reach the image at all" question without two more
 * Engines with world_ui_enabled on and off: a few hundred pixels of the button's
 * HIGHLIGHT colour can only be there if the canvas laid out, emitted, drew as 3D geometry and
 * composited over the frame. The A/B is the same pointer logic either way, differing only in
 * WHERE it points, so a failure means the mapping is wrong rather than that the UI is missing.
 *
 * Both frames come from ONE Engine via Engine::set_cursor_override(): world_ui_enabled is
 * startup-fixed, but the pointer is not, and two 1920x1080 Engines to move the mouse 250 pixels
 * was the single most expensive thing this suite did.
 *
 * Captured at DISPLAY resolution (low_res = false). The world UI is not part of the low-res
 * image: it renders into its own layer and composites after AA and tilt shift, so
 * low_res_color_image() is scene-only by construction (see
 * PixelRenderPipeline::overlay_target_) and a low-res capture would compare two frames that
 * genuinely are identical.
 *
 * FIXED_DT is a tiny 0.5 ms rather than 0: the scene's camera auto-orbits (so this keeps the
 * button essentially still across the ticks) but Button's colour chase and the EventSystem
 * still need the frame to advance at all. aa_mode stays at its "off" default deliberately --
 * TAA's non-reprojecting history clamp would ghost a canvas moving under an orbiting camera
 * and make the comparison depend on frame count.
 */
COOPA_TEST(world_canvas_button_lights_up_under_the_pointer) {
    ScopedEnv fixed_dt("FIXED_DT", "0.0005");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/world_canvas_test/scene.yaml", 1920, 1080, 1440, 960);

    toy::core::Engine engine(std::move(config));

    // Scene colours: normal (0.20, 0.42, 0.30), highlighted (0.45, 0.92, 0.60). World UI is
    // composited AFTER tonemapping, so these reach the framebuffer very nearly 1:1. Counting
    // near-matches beats sampling one hardcoded coordinate: the saved image's size depends on
    // config, and the camera orbits, so a fixed pixel index is exactly the assertion that rots.
    auto count_highlight = [](const Frame& f) { return count_near_color(f, 115, 235, 153, 26); };

    // Three ticks after each pointer move: Button's colour chase runs in update(), a phase
    // EARLIER than the late_update() that detects the hover, so the tint can only land from the
    // second frame onward however short fade_duration is.
    // The two pointer positions below are authored against a 1920x1080 framebuffer. A window
    // can come up with a different one -- Retina doubles it, and a window larger than the
    // screen is clamped -- so re-express each point through the same letterbox maths the
    // engine's own pointer ray uses (render::compute_display_rect). macOS only: elsewhere the
    // points are used exactly as authored.
#ifdef __APPLE__
    const Frame probe = engine.capture_image(/*low_res=*/false);  // only its extent is used
    auto to_window = [&](float x, float y) {
        const uint32_t rw = 1440, rh = 960;
        const auto ref = toy::render::compute_display_rect(engine.render_config(), 1920, 1080, rw, rh);
        const auto act = toy::render::compute_display_rect(engine.render_config(), probe.width, probe.height, rw, rh);
        const float u = (x - static_cast<float>(ref.x)) / static_cast<float>(ref.w);
        const float v = (y - static_cast<float>(ref.y)) / static_cast<float>(ref.h);
        return glm::vec2(static_cast<float>(act.x) + u * static_cast<float>(act.w),
                         static_cast<float>(act.y) + v * static_cast<float>(act.h));
    };
#else
    auto to_window = [](float x, float y) { return glm::vec2(x, y); };
#endif

    engine.set_cursor_override(to_window(1097.0f, 407.0f)); // the Heal button, in window pixels
    tick_frames(engine, 3);
    const Frame hovered = engine.capture_image(/*low_res=*/false);

    engine.set_cursor_override(to_window(850.0f, 760.0f));  // empty floor below it
    tick_frames(engine, 3);
    const Frame idle = engine.capture_image(false);

    expect(same_extent(hovered, idle), "world canvas hover: both captures share one extent");
    if (!same_extent(hovered, idle)) return;

    const long long on_count  = count_highlight(hovered);
    const long long off_count = count_highlight(idle);

    // The button is 34x13 canvas pixels on a 150x38 canvas; at this render extent that is a few
    // hundred framebuffer pixels, comfortably clear of any stray match in the scene.
    bool ok = expect_at_least(on_count, 200,
                              "hovered world-space button lights up (canvas -> ray -> EventSystem works)");
    ok &= expect(off_count < 50, "un-hovered world-space button stays its normal colour");
    ok &= expect(on_count > off_count * 4 + 100, "hover is a large, unambiguous change, not noise");

    // The change must also be LOCAL. Nothing but the pointer differs between these two frames,
    // so a diff spanning the whole image would mean the camera (or time) moved instead -- which
    // would make the colour counts above coincidental rather than causal.
    const long long changed = count_diff(hovered, idle, /*tolerance=*/8);
    const long long pixels  = static_cast<long long>(hovered.width) * hovered.height;
    ok &= expect_at_least(changed, 200, "the hover changes a button-sized region of the frame");
    ok &= expect(changed < pixels / 20,
                 "the hover changes only a small part of the frame (the camera and clock held still)");

    if (!ok) {
        dump_frame(hovered, "world_canvas_hovered");
        dump_frame(idle, "world_canvas_idle");
    }
}

/**
 * @brief The UI showcase in the game engine: the HUD is a UI asset placed as `prefab: ui/hud`,
 *        its composites (StatBar, Hotbar...) expand from YAML with the theme, HealthDriver binds
 *        the authored Health bar BY NAME and drains it, and the canvas draws over the frame.
 */
COOPA_TEST(ui_showcase_hud_binds_by_name_and_draws) {
    ScopedEnv fixed_dt("FIXED_DT", "0.05");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/ui_showcase/scene.yaml", 1280, 720, 1280, 720);
    toy::core::Engine engine(std::move(config));
    for (int i = 0; i < 3; ++i) engine.tick();
    coopa::ui::UiHandle hud(engine.scene().find_object("hud"));
    expect(hud && hud.find<coopa::ui::CanvasComponent>("") != nullptr, "ui_showcase: the HUD prefab is in the scene with its canvas");
    expect(hud.find<coopa::ui::ProgressBar>("Health") && hud.find<coopa::ui::InventoryGrid>("Hotbar") && hud.has("QuestTitle"),
           "ui_showcase: its composites expanded (Health bar, Hotbar, quest text)");
    const float before = hud.get<float>("Health", -1.0f);
    for (int i = 0; i < 40; ++i) engine.tick();
    const float after = hud.get<float>("Health", -1.0f);
    expect(before > 0.0f && after < before, "ui_showcase: HealthDriver drives the authored bar by name (" +
           std::to_string(before) + " -> " + std::to_string(after) + ")");
    auto* plate = engine.scene().find_object("Nameplate");
    expect(plate && plate->get_component<coopa::ui::CanvasComponent>() && plate->get_component<coopa::ui::CanvasComponent>()->is_world_space(),
           "ui_showcase: the hero carries a world-space nameplate");
    const Frame f = engine.capture_image(false);
    expect(count_near_color(f, 209, 51, 56, 30) > 50, "ui_showcase: the health bar's red is on screen");
}
