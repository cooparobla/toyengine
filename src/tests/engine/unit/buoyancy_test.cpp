/**
 * @file buoyancy_test.cpp
 * @brief Buoyancy against Archimedes: boxes float at their density ratio and settle (and sleep),
 *        dense ones sink at a drag-limited speed, dry bodies fall freely, waves heave floaters,
 *        currents carry them, and the simulation range freezes far floaters at their waterline
 *        with hysteresis.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "engine/support/checks.h"
#include "engine/support/water_fixtures.h"

COOPA_TEST_SUITE("buoyancy");

using namespace toy::test;

/** @brief Archimedes: a box floats with mass/(rho V) of it under water, and settles there. */
COOPA_TEST(box_floats_at_its_density_ratio) {
    BuoyScene bs = make_water_scene();
    // Flat slabs (2 x 2 x 0.5 m = 2 m^3), not cubes: a cube of density 0.25-0.75 is unstable
    // flat-side-up and floats on an edge (correctly -- that is real hydrostatics), which would
    // make the waterline assertions below depend on the tilt it settles at.
    SceneObject* half = add_box(bs, "half", glm::vec3(-5.0f, 0.0f, 1.0f), glm::vec3(2.0f, 2.0f, 0.5f), 1000.0f);
    SceneObject* light = add_box(bs, "light", glm::vec3(5.0f, 0.0f, 1.0f), glm::vec3(2.0f, 2.0f, 0.5f), 500.0f);
    start(bs);
    run(bs, 12.0f);

    auto* bh = half->get_component<toy::water::Buoyancy>();
    auto* bl = light->get_component<toy::water::Buoyancy>();
    expect(bh->resolved.size() == 8u, "buoyancy: a box collider generates a 2x2x2 pontoon lattice");
    expect_near(bh->submerged_fraction, 0.5f, 0.03f, "buoyancy: 500 kg/m^3 floats half submerged");
    expect_near(com_of(half).z, 0.0f, 0.03f, "buoyancy: ...which puts its centre on the waterline");
    expect_near(bl->submerged_fraction, 0.25f, 0.03f, "buoyancy: 250 kg/m^3 floats a quarter submerged");
    expect_near(com_of(light).z, 0.125f, 0.03f, "buoyancy: ...riding a quarter of its 0.5 m height higher");
    auto* rb = half->get_component<coopa::physx::components::RigidbodyComponent>();
    expect(glm::length(rb->velocity()) < 0.02f, "buoyancy: the bob has damped out on calm water");
    expect(rb->is_sleeping(), "buoyancy: a body at rest on calm water is allowed to fall asleep");
}

/** @brief Denser than water sinks -- but at a bounded speed, water drag doing its job. */
COOPA_TEST(dense_body_sinks_at_a_drag_limited_speed) {
    BuoyScene bs = make_water_scene();
    SceneObject* stone = add_box(bs, "stone", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 2400.0f);
    start(bs);
    run(bs, 4.0f);
    auto* rb = stone->get_component<coopa::physx::components::RigidbodyComponent>();
    expect(com_of(stone).z < -3.0f, "buoyancy: a 2400 kg/m^3 block sinks");
    expect(rb->velocity().z > -5.0f, "buoyancy: ...at a drag-limited speed, not in free fall");
    expect(stone->get_component<toy::water::Buoyancy>()->submerged_fraction > 0.99f,
           "buoyancy: ...fully submerged");
}

/** @brief Waves move floating things: a calm-water body stays put, a wavy one keeps moving. */
COOPA_TEST(floating_box_heaves_with_the_waves) {
    BuoyScene bs = make_water_scene([](toy::water::WaterBody& w) {
        w.waves.amplitude = 0.3f;
        w.waves.wavelength = 8.0f;
    });
    SceneObject* box = add_box(bs, "box", glm::vec3(0.0f, 0.0f, 0.5f), glm::vec3(1.0f), 400.0f);
    start(bs);
    run(bs, 6.0f);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 180; ++i) {
        run(bs, 1.0f / 60.0f);
        lo = std::min(lo, com_of(box).z);
        hi = std::max(hi, com_of(box).z);
    }
    expect(hi - lo > 0.2f, "buoyancy: a floating box heaves with the waves (range " + std::to_string(hi - lo) + " m)");
    toy::water::WaterSample s;
    expect(bs.water->sample(glm::vec2(com_of(box)), s), "water system: samples the surface under the box");
    expect(std::fabs(com_of(box).z - s.surface_height) < 0.5f, "buoyancy: the box stays at the surface it rides");
}

/** @brief A flowing body carries a floating box downstream, along the derived current. */
COOPA_TEST(current_carries_a_floater_downstream) {
    BuoyScene bs = make_water_scene([](toy::water::WaterBody& w) {
        std::vector<glm::vec3> pos;
        std::vector<glm::vec2> uv;
        std::vector<uint32_t> idx;
        make_strip(60.0f, 8.0f, 60, 8, [](float x) { return -0.03f * x; }, pos, uv, idx);
        w.mode = toy::water::WaterMode::Flowing;
        w.set_geometry(pos, uv, idx);
    });
    SceneObject* box = add_box(bs, "drifter", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.6f), 60.0f);
    start(bs);
    run(bs, 5.0f);
    toy::water::WaterSample s;
    expect(bs.water->sample(glm::vec2(10.0f, 0.0f), s), "water system: the flowing strip is sampleable");
    expect(s.flow.x > 0.5f, "water system: the strip's current runs downhill (+X)");
    const glm::vec3 p = com_of(box);
    expect(p.x > 9.0f, "buoyancy: the current carried the box downstream (x = " + std::to_string(p.x) + ")");
    expect(std::fabs(p.y) < 1.0f, "buoyancy: ...along the current, not across it");
}

/** @brief A floater outside the simulation range is frozen -- but one that is awake out there
 *         (dropped in) floats at its density ratio rather than sinking, then goes to sleep. */
COOPA_TEST(out_of_range_floater_freezes_at_its_waterline) {
    BuoyScene bs = make_water_scene();
    SceneObject* slab = add_box(bs, "far_slab", glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(2.0f, 2.0f, 0.5f), 1000.0f);
    start(bs);
    bs.water->set_focus(glm::vec3(1000.0f, 0.0f, 0.0f));
    run(bs, 10.0f);
    auto* b = slab->get_component<toy::water::Buoyancy>();
    auto* rb = slab->get_component<coopa::physx::components::RigidbodyComponent>();
    expect(!b->simulated, "frozen floater: 1 km away is outside the simulation range");
    expect(bs.water->active_buoyant_count() == 0u, "frozen floater: ...so full buoyancy skips it");
    expect_near(com_of(slab).z, 0.0f, 0.08f, "frozen floater: ...yet it floats at its waterline (z " +
                                                  std::to_string(com_of(slab).z) + ")");
    expect(b->in_water && b->submerged_fraction > 0.4f && b->submerged_fraction < 0.6f,
           "frozen floater: ...half submerged, like a simulated one");
    expect(rb->is_sleeping(), "frozen floater: ...and is put to sleep once settled");

    // Back in range: full buoyancy again, and it stays where it is on calm water.
    const glm::vec3 before = com_of(slab);
    bs.water->set_focus(glm::vec3(10.0f, 0.0f, 0.0f));
    run(bs, 2.0f);
    expect(b->simulated, "frozen floater: simulated again once the focus comes near");
    expect(glm::distance(com_of(slab), before) < 0.05f, "frozen floater: ...without a jump");
}

/** @brief The simulation range has hysteresis: in at r, out only past r * hysteresis. */
COOPA_TEST(simulation_range_has_hysteresis) {
    BuoyScene bs = make_water_scene();
    SceneObject* box = add_box(bs, "box", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 400.0f);
    start(bs);
    const float r = bs.water->settings().sim_radius;
    const float mid = r * (1.0f + 0.5f * (bs.water->settings().sim_hysteresis - 1.0f)) + 1.0f;
    auto* b = box->get_component<toy::water::Buoyancy>();
    auto at = [&](float d) {
        bs.water->set_focus(glm::vec3(d, 0.0f, 0.0f));
        run(bs, 2.0f / 60.0f);
        return b->simulated;
    };
    expect(at(r * 0.5f), "sim range: well inside is simulated");
    expect(at(mid), "sim range: between r and r * hysteresis stays simulated");
    expect(!at(r * 2.0f), "sim range: far outside is frozen");
    expect(!at(mid), "sim range: coming back between r and r * hysteresis stays frozen");
    expect(at(r * 0.5f), "sim range: inside again is simulated");
}
