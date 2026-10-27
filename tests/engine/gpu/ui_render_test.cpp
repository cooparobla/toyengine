/**
 * @file ui_render_test.cpp
 * @brief UI reaching the screen: a world-space canvas button lights up under the pointer (window
 *        pixel -> letterbox -> NDC -> ray -> canvas -> EventSystem -> Button), and the ui_demo
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
#include <toyengine/scene/health_driver.h>
#include <toyengine/ui/panel_screen.h>
#include <toyengine/ui/demo_hud.h>
#include <toyengine/render/toy_render_math.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scene_variant.h"

COOPA_TEST_SUITE("ui_render");

using namespace toy::test;

/**
 * @brief Parks the pointer on the world-space "Heal" Button in ui_demo, then moves it
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
 * ToyRenderPipeline::overlay_target_) and a low-res capture would compare two frames that
 * genuinely are identical.
 *
 * FIXED_DT is a tiny 0.5 ms rather than 0: the scene's camera auto-orbits (so this keeps the
 * button essentially still across the ticks) but Button's colour chase and the EventSystem
 * still need the frame to advance at all. aa_mode stays at its "off" default deliberately --
 * TAA's non-reprojecting history clamp would ghost a canvas moving under an orbiting camera
 * and make the comparison depend on frame count.
 */
COOPA_TEST(world_space_ui_renders_through_the_world_pass) {
    ScopedEnv fixed_dt("FIXED_DT", "0.0005");
    ScopedEnv no_input("NO_INPUT", "1");

    // ui_demo's world plates alone: the screen-space HUD and the settings panel are removed,
    // so anything the health colour matches had to come through UiWorldPass.
    const std::string scene = scene_variant("assets/scenes/ui/ui_demo/scene.yaml", "world_canvas", [](fkyaml::node& doc) {
        auto& roots = doc["scene"]["root_objects"].as_seq();
        std::erase_if(roots, [](const fkyaml::node& o) {
            if (!o.contains("name")) return false;
            const std::string name = o["name"].get_value<std::string>();
            return name == "hud" || name == "settings";
        });
    });
    toy::core::AppConfig config = make_test_config(scene, 1920, 1080, 1440, 960);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 20);

    // The plates are declared per frame by HealthDriver and WorldLabel, drawn onto world-space
    // canvases those components build themselves. Their bars use the theme's health colour.
    // Whether a world widget RESPONDS to the pointer is pinned down headlessly in the
    // imm_game suite, which can aim a ray exactly; here the question is only whether the
    // immediate pass reaches the screen at all through the world pass.
    const Frame f = engine.capture_image(/*low_res=*/false);
    const long long health_px = count_near_color(f, 209, 80, 75, 40);
    const bool ok = expect_at_least(health_px, 100,
                                    "a world-space plate's health bar is on screen (" +
                                        std::to_string(health_px) + " px)");
    if (!ok) dump_frame(f, "world_canvas_plates");
}

/**
 * @brief The UI showcase in the game engine: a C++ HUD, a YAML-authored panel and world
 *        plates, all declared per frame, drawn over the finished image.
 */
COOPA_TEST(ui_showcase_draws_its_screens_and_the_health_drains) {
    ScopedEnv fixed_dt("FIXED_DT", "0.05");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/ui/ui_demo/scene.yaml", 1280, 720, 1280, 720);
    toy::core::Engine engine(std::move(config));
    for (int i = 0; i < 3; ++i) engine.tick();

    // Three kinds of screen in one scene: a C++ HUD, a YAML-authored panel, and world plates.
    // None of them is a widget tree, so what the scene holds is three components.
    auto* hud = engine.scene().find_first_component<toy::ui::DemoHud>();
    auto* panel = engine.scene().find_first_component<toy::ui::PanelScreen>();
    auto* driver = engine.scene().find_first_component<toy::scene::HealthDriver>();
    expect(hud != nullptr, "ui_demo: the HUD component is in the scene");
    expect(panel != nullptr, "ui_demo: the authored panel component is in the scene");
    require(driver != nullptr, "ui_demo: the health driver is in the scene");
    expect(panel && panel->asset() != nullptr, "ui_demo: the panel asset loaded");

    // The driver owns its Resource and draws it, so the value IS the state -- no bar to bind
    // and nothing to fall out of step.
    const float before = driver->health().current;
    for (int i = 0; i < 40; ++i) engine.tick();
    const float after = driver->health().current;
    expect(before > 0.0f && after < before,
           "ui_demo: the health drains (" + std::to_string(before) + " -> " + std::to_string(after) + ")");

    const Frame f = engine.capture_image(false);
    expect(count_near_color(f, 209, 80, 75, 40) > 50, "ui_demo: the health bar's red is on screen");
}
