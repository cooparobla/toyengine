/**
 * @file water_parallel_test.cpp
 * @brief The water system's job-parallel paths (buoyancy per floater, bake, query, tiles) give
 *        bit-identical results to running on one worker, on water_stress_demo.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <glm/gtc/quaternion.hpp>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("water_parallel");

using namespace toy::test;

/**
 * @brief The water system's job-parallel paths (buoyancy per floater, bake geometry, query and
 *        tiles) give bit-identical results to running inline: water_stress_demo (200 floaters, a
 *        1000 m ocean -- every parallel path engages) simulated on one worker and on many ends
 *        with every floater in exactly the same pose, and the lake surfaces sample identically.
 */
COOPA_TEST(one_and_eight_workers_give_identical_water) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    struct Run {
        std::vector<glm::vec3> pos;
        std::vector<glm::quat> rot;
        std::vector<float> surface;
        int active = 0;
    };
    auto run = [](unsigned int workers) {
        toy::core::AppConfig config = make_test_config("assets/scenes/water/water_stress_demo/scene.yaml", 320, 180, 160, 90);
        config.jobs.worker_threads = workers;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, 90);
        Run r;
        auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
        if (water) r.active = static_cast<int>(water->active_buoyant_count());
        for (auto* b : engine.scene().get_components<toy::water::Buoyancy>()) {
            const auto& t = b->owner->get_transform()->transform();
            r.pos.push_back(t.position());
            r.rot.push_back(t.rotation_quat());
        }
        if (water) {
            for (int i = 0; i < 64; ++i) {
                toy::water::WaterSample s;
                const glm::vec2 p(-60.0f + 2.0f * static_cast<float>(i), 10.0f + 0.5f * static_cast<float>(i));
                r.surface.push_back(water->sample(p, s) ? s.surface_height : -1e9f);
            }
        }
        return r;
    };
    const Run serial = run(1);
    const Run parallel = run(8);
    expect(serial.pos.size() >= 100 && serial.active >= static_cast<int>(32),
           "water parallel: the scene exercises the parallel buoyancy path (" + std::to_string(serial.active) + " active)");
    bool same = serial.pos.size() == parallel.pos.size() && serial.active == parallel.active;
    for (std::size_t i = 0; same && i < serial.pos.size(); ++i) {
        same = serial.pos[i] == parallel.pos[i] && serial.rot[i] == parallel.rot[i];
    }
    expect(same, "water parallel: every floater ends in exactly the same pose on 1 and 8 workers");
    expect(serial.surface == parallel.surface, "water parallel: the baked surfaces sample identically");
}
