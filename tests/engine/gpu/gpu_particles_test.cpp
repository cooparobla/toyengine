/**
 * @file gpu_particles_test.cpp
 * @brief `simulation: gpu` particles (render/passes/gpu_particle_pass.h): the compute simulation
 *        settles at rate x lifetime inside the CPU's bounds, is deterministic under FIXED_DT, covers
 *        the screen like the CPU path, and systems with sub emitters fall back to the CPU.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/render/particle_types.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("gpu_particles");

using namespace toy::test;

namespace gpu_particles_test {

/** @brief A camera looking at one sphere of static red dots (and `extra` systems), written to a
 *         scratch scene. `sim` is the ParticleSystem's simulation value. */
std::string write_scene(const std::string& name, const std::string& sim, const std::string& extra = "") {
    namespace fs = std::filesystem;
    const fs::path dir = coopa::test::scratch_dir() / ("gpu_particles_" + name);
    fs::create_directories(dir);
    std::ofstream(dir / "scene.yaml") <<
        "format: blender\nscene:\n  scene_name: " << name << "\n  root_objects:\n"
        "    - name: Camera\n      components:\n        - type: Transform\n          position: {x: 0, y: -8, z: 0}\n"
        "          rotation: {x: 90, y: 0, z: 0}\n        - type: Camera\n          main: true\n"
        "    - name: Dots\n      components:\n        - type: Transform\n"
        "        - type: ParticleSystem\n          simulation: " << sim << "\n"
        "          max_particles: 4000\n          shape: sphere\n          radius: 1.6\n          rate: 200\n"
        "          start_lifetime: [3.0, 5.0]\n          start_speed: 0.0\n          start_size: 0.12\n"
        "          start_color: {r: 1.0, g: 0.0, b: 0.0, a: 1.0}\n          sprite: circle\n          softness: 0.0\n"
        "          emissive: 2.0\n          sort: distance\n" << extra;
    return (dir / "scene.yaml").string();
}

toy::core::AppConfig config_for(const std::string& scene, bool gpu_enabled) {
    toy::core::AppConfig config = make_test_config(scene, 320, 180, 320, 180);
    config.render.transparency_enabled = true;
    config.particles.gpu_enabled = gpu_enabled;
    return config;
}

toy::particles::ParticleSystem* system(toy::core::Engine& engine, const char* name) {
    auto* o = engine.scene().find_object(name);
    return o ? o->get_component<toy::particles::ParticleSystem>() : nullptr;
}

/** @brief Pixels where the red dots are (red-dominant over the neutral sky). */
long long red_coverage(const Frame& f) {
    long long n = 0;
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* px = &f.pixels[i * f.channels];
        if (px[0] > 120 && px[0] > px[1] + 60 && px[0] > px[2] + 60) ++n;
    }
    return n;
}

} // namespace gpu_particles_test

/**
 * @brief A `simulation: gpu` system runs in compute: it is drawn, its read-back alive count settles
 *        at rate x mean lifetime, every particle stays inside the CPU's bounds; a second run of the
 *        same scene captures the same frame (alive-list order varies with atomics, so a handful of
 *        blend-rounding differences are allowed); and the same emitter on the CPU
 *        (particles.gpu_enabled: false, the kill switch) covers a similar area of the screen.
 *
 * Three Engines, all ticked 5.5 s -- past the longest lifetime -- and captured at that frame.
 */
COOPA_TEST(gpu_simulation_settles_in_bounds_deterministically_and_matches_cpu) {
    using namespace gpu_particles_test;
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::string scene = write_scene("steady", "gpu");
    const int kSteadyFrames = 330;

    Frame first;
    {
        toy::core::Engine engine(config_for(scene, true));
        if (!engine.pipeline().gpu_particle_pass()) {
            expect(true, "gpu particles: no compute on this device (CPU fallback covers it)");
            return;
        }
        tick_frames(engine, kSteadyFrames);
        first = engine.capture_image(/*low_res=*/true);
        auto* ps = system(engine, "Dots");
        expect(ps && ps->gpu_active(), "gpu particles: the system simulates on the GPU");
        if (!ps || !ps->gpu_active()) return;
        const uint32_t alive = ps->particle_count();
        expect(alive > 680 && alive < 920, "gpu particles: steady-state alive count ~ rate x lifetime = 800 (" +
                                           std::to_string(alive) + ")");
        const auto& st = engine.pipeline().particle_state();
        expect(st.gpu.size() == 1 && st.quads.size() == 1 && st.quads[0].gpu_id == ps->gpu_id(),
               "gpu particles: one job and one indirect batch reach the renderer");

        auto* gpu = engine.pipeline().gpu_particle_pass();
        gpu->request_debug_readback(ps->gpu_id());
        std::vector<toy::render::ParticleInstance> inst;
        bool got = false;
        for (int i = 0; i < 6 && !got; ++i) {
            tick_frames(engine, 1);
            got = gpu->debug_instances(ps->gpu_id(), inst);
        }
        expect(got && inst.size() > 600, "gpu particles: the alive list reads back (" + std::to_string(inst.size()) + ")");
        const glm::vec3 lo = ps->bounds_min(), hi = ps->bounds_max();
        size_t outside = 0, bad = 0;
        for (const auto& p : inst) {
            const glm::vec3 q(p.pos_size);
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !(p.pos_size.w > 0.0f)) ++bad;
            if (glm::any(glm::lessThan(q, lo)) || glm::any(glm::greaterThan(q, hi))) ++outside;
            if (glm::length(q) > 1.61f) ++outside;   // born in the sphere, never moving
        }
        expect(bad == 0, "gpu particles: every instance is finite with a positive size");
        expect(outside == 0, "gpu particles: every particle stays in the shape and the system bounds (" +
                             std::to_string(outside) + " outside)");
        const long long cov = red_coverage(first);
        expect(cov > 500, "gpu particles: the dots are on screen (" + std::to_string(cov) + " px)");
        if (cov <= 500) dump_frame(first, "gpu_particles_steady");
    }

    // Determinism: a second GPU run of the same scene, captured at the same frame.
    {
        toy::core::Engine engine(config_for(scene, true));
        tick_frames(engine, kSteadyFrames);
        const Frame again = engine.capture_image(/*low_res=*/true);
        expect(first.pixels.size() == again.pixels.size(), "gpu particles: same capture size");
        if (first.pixels.size() == again.pixels.size()) {
            size_t differ = 0;
            for (size_t i = 0; i < first.pixels.size(); ++i) {
                if (std::abs(static_cast<int>(first.pixels[i]) - static_cast<int>(again.pixels[i])) > 2) ++differ;
            }
            expect(differ <= first.pixels.size() / 1000, "gpu particles: deterministic under FIXED_DT (" +
                                                             std::to_string(differ) + " channel values differ)");
            if (differ > first.pixels.size() / 1000) { dump_frame(first, "gpu_particles_det_a"); dump_frame(again, "gpu_particles_det_b"); }
        }
    }

    // The CPU path (the kill switch) covers a similar area of the screen.
    {
        toy::core::Engine engine(config_for(scene, false));
        tick_frames(engine, kSteadyFrames);
        auto* ps = system(engine, "Dots");
        expect(ps && !ps->gpu_active(), "gpu particles: gpu_enabled false runs on the CPU");
        const long long cpu = red_coverage(engine.capture_image(/*low_res=*/true));
        const long long gpu = red_coverage(first);
        const double ratio = cpu > 0 ? static_cast<double>(gpu) / static_cast<double>(cpu) : 0.0;
        expect(cpu > 500 && ratio > 0.75 && ratio < 1.33,
               "gpu particles: CPU and GPU coverage agree (cpu " + std::to_string(cpu) + " px, gpu " +
                   std::to_string(gpu) + " px)");
    }
}

/** @brief Sub emitters are CPU-only: a gpu system with on_death, and its target, fall back to the
 *         CPU and still splash. */
COOPA_TEST(sub_emitters_fall_back_to_the_cpu) {
    using namespace gpu_particles_test;
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::string extra =
        "    - name: Drops\n      components:\n        - type: Transform\n          position: {x: 0, y: 0, z: 0.5}\n"
        "        - type: ParticleSystem\n          simulation: gpu\n          rate: 40\n          start_speed: 3.0\n"
        "          start_lifetime: 4.0\n          gravity: 1.0\n          collide: true\n          ground_height: 0.0\n"
        "          kill_on_collide: true\n          on_death:\n            - {target: Splash, count: 2}\n"
        "    - name: Splash\n      components:\n        - type: Transform\n"
        "        - type: ParticleSystem\n          simulation: gpu\n          rate: 0\n          start_lifetime: 1.0\n";
    toy::core::Engine engine(config_for(write_scene("subemit", "gpu", extra), true));
    tick_frames(engine, 120);
    auto* drops = system(engine, "Drops");
    auto* splash = system(engine, "Splash");
    expect(drops && !drops->gpu_active() && drops->gpu_fallback_warned,
           "gpu particles: a system with sub emitters falls back to the CPU, warned once");
    expect(splash && !splash->gpu_active(), "gpu particles: a sub-emitter target falls back to the CPU too");
    expect(drops && drops->particle_count() > 0 && splash && splash->particle_count() > 0,
           "gpu particles: the fallback still simulates and splashes");
}
