/**
 * @file particles_scene_test.cpp
 * @brief particles_test end to end: every system simulates, quads and mesh scatters reach the
 *        renderer, the campfire is warm on screen with no blowout or NaN blocks, and an emptied
 *        frame draws nothing.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/particle_system_runner.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("particles_scene");

using namespace toy::test;

/** @brief particles_test end to end: every system simulates, quads and mesh scatters reach the
 *         renderer, and the fire is on screen -- warm, bright, and free of NaN blowouts. */
COOPA_TEST(particles_scene_simulates_and_renders) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/particles_test/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;   // particle quads draw in the forward transparent pass
    config.render.bloom_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 90);

    auto* runner = dynamic_cast<toy::particles::ParticleSimulationSystem*>(engine.scene().find_system("Particles"));
    expect(runner != nullptr, "particles scene: Engine installed the particle system");
    if (!runner) return;
    std::size_t live = 0, empty = 0;
    for (auto* s : runner->systems()) {
        live += s->particle_count();
        if (s->particle_count() == 0) ++empty;
    }
    expect(runner->systems().size() >= 15u, "particles scene: every authored system is gathered (" +
                                                std::to_string(runner->systems().size()) + ")");
    expect(empty == 0u, "particles scene: every system has live particles after 1.5 s");
    expect(live > 800u, "particles scene: a healthy particle count (" + std::to_string(live) + ")");

    auto* mush = engine.scene().find_object("mushrooms");
    auto* ps = mush ? mush->get_component<toy::particles::ParticleSystem>() : nullptr;
    expect(ps && ps->particle_count() == 70u, "particles scene: the mushroom scatter placed all 70 on the mound");

    const auto& st = engine.pipeline().particle_state();
    expect(st.quads.size() >= 8u && st.total_quads() > 300u, "particles scene: quad batches reach the renderer");
    expect(st.meshes.size() == 2u, "particles scene: the mushroom and pebble scatters draw as instanced meshes");

    const Frame f = engine.capture_image(/*low_res=*/true);
    long long warm = 0, blown = 0, black = 0;
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* px = &f.pixels[i * f.channels];
        if (px[0] > 200 && px[1] > 110 && px[2] < px[1]) ++warm;   // fire: bright, orange-yellow
        if (px[0] > 250 && px[1] > 250 && px[2] > 250) ++blown;
        if (px[0] < 2 && px[1] < 2 && px[2] < 2) ++black;
    }
    expect(warm > 400, "particles scene: the campfire glows warm on screen (" + std::to_string(warm) + " px)");
    expect(blown < static_cast<long long>(pixels / 50), "particles scene: no white blowout (" + std::to_string(blown) + " px)");
    expect(black < static_cast<long long>(pixels / 200), "particles scene: no NaN black blocks (" + std::to_string(black) + " px)");
    if (warm <= 400 || blown >= static_cast<long long>(pixels / 50)) dump_frame(f, "particles_scene");

    // A frame with every particle system gone draws no quads at all -- the transparent pass
    // must still run cleanly with nothing but particles to skip.
    for (auto* s : runner->systems()) s->stop(true);
    tick_frames(engine, 2);
    expect(engine.pipeline().particle_state().total_quads() == 0u, "particles scene: stopped and cleared, nothing is drawn");
}
