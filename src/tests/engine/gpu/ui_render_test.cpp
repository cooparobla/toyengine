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
#include <toyengine/ui/imm_screen.h>
#include <toyengine/render/toy_render_math.h>
#include <coopa/scene/scene_object.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scene_variant.h"

COOPA_TEST_SUITE("ui_render");

using namespace toy::test;

namespace {

/**
 * @brief A screen that draws one glass_panel() over a fixed window-pixel rect -- the minimal
 *        probe for proving the backdrop blur reaches the screen, with no scene component or
 *        registration needed (it is added directly via Scene::add_root_object(), the same way
 *        imm_game_test.cpp's headless Probe is).
 */
struct GlassProbe : toy::ui::ImmScreen {
    coopa::ui::imm::Box pane;
    std::string type_name() const override { return "GlassProbe"; }
    void draw(coopa::ui::imm::Context& ctx) override { ctx.glass_panel(pane); }
};

/**
 * @brief Mean |a-b| between adjacent pixels inside [x0,x1)x[y0,y1) -- a cheap local-contrast
 *        measure. High over busy geometry, near zero over a blurred or flat-filled patch.
 *        Not in render_fixture.h: every other caller there compares whole frames or fractional
 *        bands, and this one needs an exact pixel rect it does not otherwise offer.
 */
double local_contrast(const Frame& f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    double sum = 0.0;
    long long n = 0;
    auto px = [&](uint32_t x, uint32_t y) {
        const uint8_t* p = &f.pixels[(static_cast<size_t>(y) * f.width + x) * f.channels];
        return glm::ivec3(p[0], p[1], p[2]);
    };
    for (uint32_t y = y0; y + 1 < y1; ++y) {
        for (uint32_t x = x0; x + 1 < x1; ++x) {
            const glm::ivec3 c = px(x, y), r = px(x + 1, y), d = px(x, y + 1);
            sum += std::abs(c.x - r.x) + std::abs(c.y - r.y) + std::abs(c.z - r.z);
            sum += std::abs(c.x - d.x) + std::abs(c.y - d.y) + std::abs(c.z - d.z);
            ++n;
        }
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

/** @brief Builds kitchen_sink with the given backdrop-blur flag, optionally adds a GlassProbe
 *         over `pane`, and ticks it to `frames`. One Engine per call -- the two-Engine,
 *         same-frame-count comparison this suite's other kitchen_sink tests already use,
 *         because the scene's own frame-indexed noise makes two captures from ONE Engine at
 *         different frame counts incomparable. */
Frame capture_kitchen_sink(bool backdrop_blur_enabled, bool add_probe, const coopa::ui::imm::Box& pane, int frames) {
    toy::core::AppConfig config = make_test_config("src/tests/fixtures/scenes/kitchen_sink/scene.yaml", 640, 360, 160, 90);
    config.render.ui_backdrop_blur_enabled = backdrop_blur_enabled;
    toy::core::Engine engine(std::move(config));
    if (add_probe) {
        auto obj = std::make_unique<coopa::scene::SceneObject>("glass_probe");
        GlassProbe* probe = obj->add_component<GlassProbe>();
        probe->pane = pane;
        // Raw device pixels, not reference_resolution-scaled: `pane` is stated in the window's
        // own pixels, matching what capture_image(false) reads back. Set directly here, not in
        // an overridden start(): nothing calls SceneObject::start() on an object added via
        // add_root_object() after the scene is already running (only the per-frame
        // BehaviourSystem walk reaches it), so a field default meant to be set there would
        // silently stay at ImmScreen's own default instead.
        probe->reference_resolution = glm::vec2(0.0f, 0.0f);
        coopa::scene::SceneObject* raw = engine.scene().add_root_object(std::move(obj));
        engine.scene().adopt(*raw);
    }
    tick_frames(engine, frames);
    return engine.capture_image(/*low_res=*/false);
}

}  // namespace

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

/**
 * @brief A glass_panel() over busy geometry genuinely blurs the frame behind it, rather than
 *        drawing a flat fill or sampling garbage: local contrast inside the pane drops sharply
 *        relative to the SAME rect with no pane there, while the mean colour stays close -- the
 *        signature of "smoothed", not "replaced".
 *
 * Two Engines at the same frame count, not one Engine captured twice: kitchen_sink has its own
 * frame-indexed noise (see debug_overlay_render_test.cpp's identical caveat), so two captures
 * from one Engine at different frame counts would differ for reasons having nothing to do with
 * the pane.
 */
COOPA_TEST(glass_panel_blurs_the_backdrop_behind_it) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    const coopa::ui::imm::Box pane{200.0f, 120.0f, 240.0f, 160.0f};
    const Frame with_pane    = capture_kitchen_sink(/*backdrop_blur_enabled=*/true, /*add_probe=*/true, pane, 10);
    const Frame without_pane = capture_kitchen_sink(/*backdrop_blur_enabled=*/true, /*add_probe=*/false, pane, 10);
    require(same_extent(with_pane, without_pane), "glass_panel: both captures share one extent");

    const uint32_t x0 = static_cast<uint32_t>(pane.x), y0 = static_cast<uint32_t>(pane.y);
    const uint32_t x1 = static_cast<uint32_t>(pane.right()), y1 = static_cast<uint32_t>(pane.bottom());
    const double contrast_with    = local_contrast(with_pane, x0, y0, x1, y1);
    const double contrast_without = local_contrast(without_pane, x0, y0, x1, y1);
    const bool contrast_ok = expect(contrast_with < contrast_without * 0.5,
        "glass_panel: local contrast under the pane drops sharply (with " +
            std::to_string(contrast_with) + " vs without " + std::to_string(contrast_without) + ")");

    const glm::vec3 mean_with    = band_mean(with_pane, pane.y / with_pane.height, pane.bottom() / with_pane.height,
                                             pane.x / with_pane.width, pane.right() / with_pane.width);
    const glm::vec3 mean_without = band_mean(without_pane, pane.y / without_pane.height, pane.bottom() / without_pane.height,
                                             pane.x / without_pane.width, pane.right() / without_pane.width);
    const double mean_delta = glm::length(glm::dvec3(mean_with) - glm::dvec3(mean_without));
    const bool mean_ok = expect(mean_delta < 60.0,
        "glass_panel: mean colour under the pane stays close to the unblurred backdrop (delta " +
            std::to_string(mean_delta) + ")");

    if (!contrast_ok || !mean_ok) {
        dump_frame(with_pane, "glass_panel_with");
        dump_frame(without_pane, "glass_panel_without");
    }
}

/**
 * @brief With the feature disabled, glass_panel() still renders -- the documented flat-fill
 *        fallback, not a crash or a quad sampling an unset backdrop.
 */
COOPA_TEST(glass_panel_degrades_to_a_flat_fill_when_backdrop_blur_is_disabled) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    const coopa::ui::imm::Box pane{200.0f, 120.0f, 240.0f, 160.0f};
    const Frame f = capture_kitchen_sink(/*backdrop_blur_enabled=*/false, /*add_probe=*/true, pane, 10);

    expect(count_nonblack(f) > 1000, "glass_panel (disabled): the frame rendered something");
    const double contrast = local_contrast(f, static_cast<uint32_t>(pane.x), static_cast<uint32_t>(pane.y),
                                           static_cast<uint32_t>(pane.right()), static_cast<uint32_t>(pane.bottom()));
    expect(contrast < 5.0, "glass_panel (disabled): the pane is a flat fallback fill (contrast " +
                                std::to_string(contrast) + ")");
}

/**
 * @brief The whole feature stack landing in one real scene: glass_ui_demo loads with its three
 *        default panels and its always-visible dock, the backdrop blur it turns on in its own
 *        settings reaches the screen, and the glass theme's gradient/blob colours are on it.
 *
 * Window and reference_resolution both land on 1920x1080 (ImmScreen's default, which
 * QuadrantDock does not override), so the dock's tune()'d 76px right-hand strip
 * (quadrant_dock.cpp's kDockWidth) lands at exact device pixels with no CanvasScaler factor to
 * account for -- the same reference_resolution/raw-pixel mismatch that cost a whole debugging
 * pass on GlassProbe above is avoided by construction here, not worked around.
 */
COOPA_TEST(glass_ui_demo_scene_renders_its_dock_and_backdrop) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/ui/glass_ui_demo/scene.yaml", 1920, 1080, 1440, 960);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 20);

    const Frame f = engine.capture_image(/*low_res=*/false);
    const long long total = static_cast<long long>(f.width) * static_cast<long long>(f.height);
    const long long nonblack = count_nonblack(f);
    const bool drew_ok = expect(nonblack > total * 95 / 100,
        "glass_ui_demo: the frame is fully drawn with no black gaps (" + std::to_string(nonblack) +
            "/" + std::to_string(total) + ")");

    const double dock_contrast = local_contrast(f, 1920 - 76, 0, 1920, 1080);
    const bool dock_ok = expect(dock_contrast < 20.0,
        "glass_ui_demo: the always-visible dock strip is a flat glass pane (contrast " +
            std::to_string(dock_contrast) + ")");

    // grad_violet (#8b00ff) and grad_pink (#ff1493) are saturated hues no PBR-lit cube or floor
    // in this scene produces; finding either on screen is finding the theme's own gradient and
    // blobs, not a lit surface. Three of the four quadrants hold a glass panel by default, so
    // the gradient only shows through the one free quadrant (and the thin outer margin) -- a
    // modest pixel count, not a dominant fraction of the frame.
    const long long violet_px = count_near_color(f, 139, 0, 255, 60);
    const long long pink_px = count_near_color(f, 255, 20, 147, 60);
    const bool gradient_ok = expect(violet_px + pink_px > 150,
        "glass_ui_demo: the glass theme's gradient/blob colours are on screen (violet " +
            std::to_string(violet_px) + ", pink " + std::to_string(pink_px) + ")");

    if (!drew_ok || !dock_ok || !gradient_ok) dump_frame(f, "glass_ui_demo_scene");
}

/**
 * @brief Minimising the dock hands the screen back to the camera: the panel area becomes the
 *        3D scene again, and the dock -- the only way back in -- stays drawn over it.
 *
 * The assertion is against the scene rendered with the dock component REMOVED, not against a
 * contrast measure: "the overlay is gone" means those two frames agree over the panel area, and
 * a dumped minimised frame should be indistinguishable from the bare scene there. Contrast
 * alone would be read backwards here -- a glass panel full of text is busier than the smooth
 * sky it covers, so minimising LOWERS local contrast while doing exactly the right thing.
 *
 * reduced_motion pins the reveal to its target, so one tick settles the transition instead of
 * the assertion depending on how many frames of easing have elapsed.
 */
COOPA_TEST(minimising_the_dock_reveals_the_scene_and_keeps_the_dock) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    // `dock_present == false` strips the component entirely: the bare scene, as the camera sees
    // it, which is what a minimised overlay should be giving back.
    auto capture = [](bool dock_present, bool minimised) {
        const std::string tag = !dock_present ? "no_dock" : (minimised ? "minimised" : "shown");
        const std::string scene = scene_variant(
            "assets/scenes/ui/glass_ui_demo/scene.yaml", tag, [&](fkyaml::node& doc) {
                auto& roots = doc["scene"]["root_objects"].as_seq();
                if (!dock_present) {
                    std::erase_if(roots, [](const fkyaml::node& o) {
                        return o.contains("name") && o["name"].get_value<std::string>() == "dock";
                    });
                    return;
                }
                for (auto& o : roots) {
                    if (!o.contains("name") || o["name"].get_value<std::string>() != "dock") continue;
                    for (auto& c : o["components"].as_seq()) {
                        c["minimised"] = minimised;
                        c["reduced_motion"] = true;
                    }
                }
            });
        toy::core::AppConfig config = make_test_config(scene, 1920, 1080, 1440, 960);
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, 20);
        return engine.capture_image(/*low_res=*/false);
    };

    const Frame shown = capture(true, false);
    const Frame mini = capture(true, true);
    const Frame bare = capture(false, false);
    require(same_extent(shown, mini) && same_extent(mini, bare), "all three captures share one extent");

    // Over the top-left quadrant -- Inventory when shown, nothing when minimised.
    auto panel_area_mean = [](const Frame& f) {
        return band_mean(f, 40.0f / 1080.0f, 500.0f / 1080.0f, 40.0f / 1920.0f, 880.0f / 1920.0f);
    };
    const double mini_vs_bare = glm::length(glm::dvec3(panel_area_mean(mini)) - glm::dvec3(panel_area_mean(bare)));
    const double shown_vs_bare = glm::length(glm::dvec3(panel_area_mean(shown)) - glm::dvec3(panel_area_mean(bare)));

    const bool revealed = expect(mini_vs_bare < 2.0,
        "minimised: the panel area matches the bare scene (delta " + std::to_string(mini_vs_bare) + ")");
    const bool was_covered = expect(shown_vs_bare > 10.0,
        "shown: the same area is covered by a panel, so it does NOT match the bare scene (delta " +
            std::to_string(shown_vs_bare) + ")");

    // The dock strip itself must survive minimising: it is the only affordance that brings the
    // UI back, so a minimised frame still has to differ from the bare scene over the strip.
    const float dock_x0 = (1920.0f - 76.0f) / 1920.0f;
    const long long dock_px = count_diff(mini, bare, 12);
    const glm::vec3 dock_mini = band_mean(mini, 0.3f, 0.7f, dock_x0, 1.0f);
    const glm::vec3 dock_bare = band_mean(bare, 0.3f, 0.7f, dock_x0, 1.0f);
    const double dock_delta = glm::length(glm::dvec3(dock_mini) - glm::dvec3(dock_bare));
    const bool dock_there = expect(dock_delta > 4.0 && dock_px > 1000,
        "minimised: the dock strip is still drawn over the scene (strip delta " +
            std::to_string(dock_delta) + ", " + std::to_string(dock_px) + " px differ overall)");

    if (!revealed || !was_covered || !dock_there) {
        dump_frame(shown, "dock_shown");
        dump_frame(mini, "dock_minimised");
        dump_frame(bare, "dock_absent");
    }
}
