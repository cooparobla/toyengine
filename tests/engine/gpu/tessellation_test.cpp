/**
 * @file tessellation_test.cpp
 * @brief tess_demo: tessellated dunes displace without adding flicker, a MeshRenderer's
 *        checkbox (gated by the water tier) is what tessellates the sea, and disabling an object
 *        hides its runtime tiles.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("tessellation");

using namespace toy::test;

/** @brief tess_demo: the tessellated dunes displace (vs. the same renderer untessellated). */
COOPA_TEST(tessellation_displaces_and_follows_its_switches) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/rendering/tess_demo/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 6);
    auto* dunes = engine.scene().find_object("dunes_tess");
    auto* mr = dunes ? dunes->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(mr && mr->tessellation.enabled, "tess_demo: dunes_tess is tessellated");
    if (!mr) return;
    if (!engine.pipeline().supports_tessellation()) {
        std::cout << "         (device has no tessellation; skipping the image checks)\n";
        return;
    }
    expect(engine.pipeline().last_frame_stats().tess_draws >= 2u,
           "tess_demo: the dunes and the sea draw tessellated (" + std::to_string(engine.pipeline().last_frame_stats().tess_draws) + ")");
    const Frame tess = engine.capture_image(true);
    tick_frames(engine, 1);
    const long long tess_noise = count_diff(tess, engine.capture_image(true));
    // The same two-frame check untessellated: the scene's own frame-to-frame noise (temporal
    // effects run in this config), which tessellation must not add to -- no crack or popping.
    mr->tessellation.enabled = false;
    tick_frames(engine, 2);
    const Frame flat = engine.capture_image(true);
    tick_frames(engine, 1);
    const long long flat_noise = count_diff(flat, engine.capture_image(true));
    expect(tess_noise <= flat_noise + flat_noise / 2 + 200,
           "tess_demo: tessellation adds no flicker (" + std::to_string(tess_noise) + " vs " + std::to_string(flat_noise) + " px)");
    const long long changed = count_diff(tess, flat, 8);
    expect_at_least(changed, tess.width * tess.height / 50, "tess_demo: tessellation + the displacement map change the dunes");
    if (changed < tess.width * tess.height / 50) { dump_frame(tess, "tess_on"); dump_frame(flat, "tess_off"); }

    // The sea: its MeshRenderer's checkbox is what tessellates its tiles; the tier only gates.
    auto* sea = engine.scene().find_object("sea");
    auto* sea_mr = sea ? sea->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(sea_mr != nullptr, "tess_demo: the sea has a MeshRenderer");
    if (!sea_mr) return;
    tick_frames(engine, 1);
    expect(engine.pipeline().last_frame_stats().tess_draws >= 1u, "tess_demo: the sea's tiles tessellate (its checkbox is on)");
    sea_mr->tessellation.enabled = false;
    tick_frames(engine, 2);
    expect(engine.pipeline().last_frame_stats().tess_draws == 0u,
           "tess_demo: unchecking the sea's tessellation stops it (" + std::to_string(engine.pipeline().last_frame_stats().tess_draws) + ")");
    sea_mr->tessellation.enabled = true;
    engine.render_config().water_quality = toy::render::RenderQuality::Low;
    tick_frames(engine, 3);
    expect(engine.pipeline().last_frame_stats().tess_draws == 0u, "tess_demo: water_quality low gates the sea's tessellation off");
    expect(sea_mr->tessellation.enabled, "tess_demo: ...without touching its authored checkbox");
    engine.render_config().water_quality = toy::render::RenderQuality::High;
    tick_frames(engine, 3);

    // Disabling the sea object hides its tiles (runtime children) with it.
    const Frame with_sea = engine.capture_image(true);
    sea->set_active(false);
    tick_frames(engine, 2);
    const Frame without_sea = engine.capture_image(true);
    expect_at_least(count_diff(with_sea, without_sea, 8), with_sea.width * with_sea.height / 40,   // the sea is ~5% of the frame
                    "tess_demo: disabling the sea hides the water");
    const long long renderers_off = engine.pipeline().last_frame_stats().renderers;
    sea->set_active(true);
    tick_frames(engine, 2);
    expect(engine.pipeline().last_frame_stats().renderers > renderers_off, "tess_demo: ...and enabling it shows it again");
}
