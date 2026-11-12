/**
 * @file particle_simulation_test.cpp
 * @brief Particle simulation: ground collision and sub emitters, precipitation's wrap box and
 *        ground field (roofs stay dry, landings splash along the surface normal), simulation
 *        space, render prep sorting, and the job-split step matching the serial one bit for bit.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/particles/particle_math.h>
#include <toyengine/particles/particle_shape.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/particle_system_runner.h>
#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <toyengine/render/particle_types.h>

#include "engine/support/checks.h"
#include "engine/support/particle_fixtures.h"

COOPA_TEST_SUITE("particle_simulation");

using namespace toy::test;

COOPA_TEST(ground_collision_kills_and_spawns_sub_emitters) {
    using namespace toy::particles;
    PScene ps;
    auto* rain = add_system(ps, "rain", glm::vec3(0, 0, 3), [](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Box;
        s.settings.shape.box = glm::vec3(1.0f);
        s.settings.start_speed = 0.0f;
        s.settings.gravity = 1.0f;
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.collide = true;
        s.settings.ground_height = 0.0f;
        s.settings.kill_on_collide = true;
        s.settings.on_death.push_back({"splash", Range(2.0f), 0.0f});
        s.settings.bursts.push_back({0.0f, Range(20.0f), 1, 1.0f, 1.0f});
    });
    auto* splash = add_system(ps, "splash", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 0.0f;
        s.settings.start_speed = 0.0f;
        s.settings.start_lifetime = 100.0f;
    });
    auto* bouncy = add_system(ps, "bouncy", glm::vec3(5, 0, 2), [](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Point;
        s.settings.start_speed = 0.0f;
        s.settings.gravity = 1.0f;
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.collide = true;
        s.settings.bounce = 0.5f;
        s.settings.bursts.push_back({0.0f, Range(1.0f), 1, 1.0f, 1.0f});
    });
    start(ps);
    run(ps, 2.0f);
    expect(rain->particle_count() == 0u, "collision: every droplet died on the ground");
    expect(splash->particle_count() == 40u, "sub emitters: each death spawned 2 splashes (" +
                                                std::to_string(splash->particle_count()) + ")");
    bool at_ground = true;
    for (const auto& p : splash->pool().pos) at_ground &= std::abs(p.z) < 0.2f;
    expect(at_ground, "sub emitters: splashes are born where the droplets died");
    expect(bouncy->particle_count() == 1u && bouncy->pool().pos[0].z >= 0.0f,
           "collision: a bouncing particle never goes through the ground");
}

/**
 * @brief Precipitation: a wrap box keeps particles around a moving emitter (rain that keeps up
 *        with the camera), a GroundField stops them on a roof instead of the ground -- none born
 *        or wrapped in under it, the splash sits on the roof -- and on_death_collision_only
 *        splashes landings only, never lifetime deaths in mid-air.
 */
COOPA_TEST(wrap_box_and_ground_field_shape_precipitation) {
    using namespace toy::particles;
    // A 4 x 4 m roof at z = 5 over [0, 4] x [0, 4]; everything else falls to the plane at 0.
    auto field = std::make_shared<GroundField>();
    field->origin = glm::vec2(-20.0f);
    field->cell = 1.0f;
    field->nx = field->ny = 40;
    field->heights.assign(40 * 40, std::numeric_limits<float>::quiet_NaN());
    for (int j = 20; j < 24; ++j) for (int i = 20; i < 24; ++i) field->heights[static_cast<size_t>(j) * 40 + i] = 5.0f;
    field->fallback = 0.0f;
    auto in_roof = [](const glm::vec3& p) { return p.x >= 0.0f && p.x < 4.0f && p.y >= 0.0f && p.y < 4.0f; };

    PScene ps;
    auto* rain = add_system(ps, "rain", glm::vec3(0, 0, 8), [&](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Box;
        s.settings.shape.box = glm::vec3(20.0f, 20.0f, 16.0f);
        s.settings.wrap_box = glm::vec3(20.0f, 20.0f, 16.0f);
        s.settings.start_speed = 0.0f;
        s.settings.velocity = glm::vec3(0.0f, 0.0f, -10.0f);
        s.settings.rate = 3000.0f;
        s.settings.max_particles = 20000;
        s.settings.start_lifetime = 50.0f;
        s.settings.collide = true;
        s.settings.kill_on_collide = true;
        s.settings.on_death_collision_only = true;
        s.settings.on_death.push_back({"splash", Range(1.0f), 0.0f});
        s.ground_field = field;
    });
    auto* splash = add_system(ps, "splash", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 0.0f;
        s.settings.start_speed = 0.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.max_particles = 100000;
    });
    // High above anything: lifetime deaths only -- which must not splash.
    auto* air = add_system(ps, "air", glm::vec3(0, 0, 200), [](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Point;
        s.settings.start_speed = 0.0f;
        s.settings.rate = 200.0f;
        s.settings.start_lifetime = 0.1f;
        s.settings.collide = true;
        s.settings.kill_on_collide = true;
        s.settings.on_death_collision_only = true;
        s.settings.on_death.push_back({"air_splash", Range(1.0f), 0.0f});
    });
    auto* air_splash = add_system(ps, "air_splash", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 100.0f;
    });
    start(ps);
    run(ps, 1.5f);
    (void)air;
    expect(rain->particle_count() > 1000u, "wrap: the box is full of rain (" + std::to_string(rain->particle_count()) + ")");
    bool dry = true, roof_splash = false, ground_splash = false, under_roof_splash = false;
    for (const auto& p : rain->pool().pos) dry &= !(in_roof(p) && p.z < 5.0f);
    for (const auto& p : splash->pool().pos) {
        if (in_roof(p) && std::abs(p.z - 5.0f) < 0.1f) roof_splash = true;
        if (!in_roof(p) && std::abs(p.z) < 0.1f) ground_splash = true;
        if (in_roof(p) && p.z < 4.0f) under_roof_splash = true;
    }
    expect(dry, "ground field: nothing under the roof (none born there, none fall through)");
    expect(roof_splash && ground_splash, "ground field: drops splash on the roof and on the ground around it");
    expect(!under_roof_splash, "ground field: no splash under the roof");
    // Surfaces opt in: with the roof marked as taking no splashes, drops still stop on it, silently.
    {
        auto quiet = std::make_shared<GroundField>(*field);
        quiet->splash.assign(40 * 40, 1);
        for (int j = 20; j < 24; ++j) for (int i = 20; i < 24; ++i) quiet->splash[static_cast<size_t>(j) * 40 + i] = 0;
        rain->ground_field = quiet;
        run(ps, 2.0f / 60.0f);   // sub-emitter spawns land a frame late: let the old field's drain
        const size_t before = splash->particle_count();
        run(ps, 0.5f);
        bool roof_quiet = true, ground_more = splash->particle_count() > before;
        for (size_t k = before; k < splash->pool().pos.size(); ++k) {
            const glm::vec3& p = splash->pool().pos[k];
            roof_quiet &= !(in_roof(p) && std::abs(p.z - 5.0f) < 0.1f);
        }
        bool dry2 = true;
        for (const auto& p : rain->pool().pos) dry2 &= !(in_roof(p) && p.z < 5.0f);
        expect(roof_quiet && ground_more && dry2, "splash flags: no splash on a quiet roof, which still keeps the rain off below");
        rain->ground_field = field;
    }
    expect(air_splash->particle_count() == 0u, "on_death_collision_only: lifetime deaths in mid-air never splash");

    // The emitter jumps 100 m (the camera teleports, or moves fast): the very next frames, the
    // rain is all around its new position, not left behind.
    rain->owner->get_transform()->transform().set_position(glm::vec3(100.0f, 0.0f, 8.0f));
    run(ps, 3.0f / 60.0f);
    bool around = rain->particle_count() > 1000u;
    for (const auto& p : rain->pool().pos) around &= std::abs(p.x - 100.0f) <= 10.0f + 1e-3f && std::abs(p.y) <= 10.0f + 1e-3f;
    expect(around, "wrap: after a 100 m jump every drop is inside the box around the emitter");

    // Toward the box's sides the drops fade out (the wrap is never seen).
    rain->prepare_render(ParticleView{glm::vec3(100.0f, 0.0f, 8.0f)});
    float centre_a = 0.0f, edge_a = 1.0f;
    for (const auto& q : rain->instances()) {
        const float d = std::max(std::abs(q.pos_size.x - 100.0f), std::abs(q.pos_size.y));
        if (d < 5.0f) centre_a = std::max(centre_a, q.color.a);
        if (d > 9.8f) edge_a = std::min(edge_a, q.color.a);
    }
    expect(centre_a > 0.9f && edge_a < 0.15f, "wrap_fade: drops near the box's sides fade out");

    // Landings carry the surface normal: a 45-degree roof's splash lies in the roof and its
    // spray kicks up off it, not into it.
    field->normals.assign(40 * 40, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 slope = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
    for (int j = 20; j < 24; ++j) for (int i = 20; i < 24; ++i) field->normals[static_cast<size_t>(j) * 40 + i] = slope;
    PScene ps2;
    auto* drop = add_system(ps2, "drop", glm::vec3(2, 2, 7), [&](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Point;
        s.settings.start_speed = 0.0f;
        s.settings.velocity = glm::vec3(0.0f, 0.0f, -10.0f);
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 10.0f;
        s.settings.collide = true;
        s.settings.kill_on_collide = true;
        s.settings.on_death.push_back({"ring", Range(1.0f), 0.0f});
        s.settings.on_death.push_back({"spray", Range(4.0f), 0.0f});
        s.settings.bursts.push_back({0.0f, Range(1.0f), 1, 1.0f, 1.0f});
        s.ground_field = field;
    });
    auto* ring = add_system(ps2, "ring", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 0.0f;
        s.settings.start_speed = 0.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.align_to_normal = true;
        s.settings.random_spin = true;
    });
    auto* spray = add_system(ps2, "spray", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.rate = 0.0f;
        s.settings.start_speed = 2.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.align_to_normal = true;
    });
    start(ps2);
    run(ps2, 0.5f);
    (void)drop;
    expect(ring->particle_count() == 1u, "aligned landing: the splash spawned");
    if (ring->particle_count() == 1u) {
        const glm::quat q = ring->pool().orient[0];
        const glm::vec3 up = q * glm::vec3(0.0f, 0.0f, 1.0f);
        expect(glm::dot(up, slope) > 0.99f, "aligned landing: the splash lies in the sloped roof");
    }
    bool off_surface = spray->particle_count() == 4u;
    for (const auto& v : spray->pool().vel) off_surface &= glm::dot(v, slope) > 0.0f;
    expect(off_surface, "aligned landing: spray leaves the surface along its normal");
}

/** @brief World-space trails stay behind a moving emitter; local space moves with it. */
COOPA_TEST(world_space_stays_and_local_space_follows) {
    using namespace toy::particles;
    PScene ps;
    auto setup = [](SimulationSpace space) {
        return [space](ParticleSystem& s) {
            s.settings.space = space;
            s.settings.shape.type = EmitShape::Point;
            s.settings.start_speed = 0.0f;
            s.settings.rate = 0.0f;
            s.settings.start_lifetime = 100.0f;
            s.settings.bursts.push_back({0.0f, Range(1.0f), 1, 1.0f, 1.0f});
        };
    };
    auto* world = add_system(ps, "world_space", glm::vec3(0), setup(SimulationSpace::World));
    auto* local = add_system(ps, "local_space", glm::vec3(0), setup(SimulationSpace::Local));
    start(ps);
    run(ps, 0.1f);
    for (auto* s : {world, local}) s->owner->get_transform()->transform().set_position(glm::vec3(4, 0, 0));
    run(ps, 0.1f);
    world->prepare_render(ParticleView{});
    local->prepare_render(ParticleView{});
    expect_near(world->instances()[0].pos_size.x, 0.0f, 1e-4f, "space: a world particle stays where it was born");
    expect_near(local->instances()[0].pos_size.x, 4.0f, 1e-4f, "space: a local particle moves with its emitter");
}

/** @brief Distance sort draws farthest first; instances carry size/colour over life. */
COOPA_TEST(render_prep_sorts_back_to_front) {
    using namespace toy::particles;
    PScene ps;
    auto* sys = add_system(ps, "sorted", glm::vec3(0), [](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Box;
        s.settings.shape.box = glm::vec3(20.0f, 20.0f, 0.0f);
        s.settings.start_speed = 0.0f;
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 2.0f;
        s.settings.start_size = 1.0f;
        s.settings.size_over_life = FloatCurve::linear(1.0f, 3.0f);
        s.settings.bursts.push_back({0.0f, Range(200.0f), 1, 1.0f, 1.0f});
        s.settings.sort = SortMode::Distance;
    });
    start(ps);
    run(ps, 1.0f);
    const glm::vec3 eye(0, -30, 10);
    sys->prepare_render(ParticleView{eye});
    const auto& inst = sys->instances();
    bool sorted = inst.size() == 200u;
    for (size_t i = 1; i < inst.size(); ++i) {
        sorted &= glm::distance(glm::vec3(inst[i - 1].pos_size), eye) >= glm::distance(glm::vec3(inst[i].pos_size), eye) - 1e-4f;
    }
    expect(sorted, "render prep: instances are back to front");
    expect_near(inst[0].pos_size.w, 2.0f, 0.05f, "render prep: size follows size_over_life at half life");
    expect_near(inst[0].misc.x, 0.5f, 0.02f, "render prep: normalized age is handed to the shader");
    const glm::vec3 lo = sys->bounds_min(), hi = sys->bounds_max();
    expect(lo.x <= -9.0f && hi.x >= 9.0f, "render prep: bounds cover the particles");
}

/** @brief The job-split step is bit-identical to the serial one (positions, ages, render data). */
COOPA_TEST(parallel_step_matches_serial_bit_for_bit) {
    using namespace toy::particles;
    auto build = [](bool serial, coopa::job::JobEngine* jobs) {
        PScene ps;
        auto cfg = [](ParticleSystem& s) {
            s.settings.shape.type = EmitShape::Sphere;
            s.settings.shape.radius = 2.0f;
            s.settings.rate = 4000.0f;
            s.settings.max_particles = 20000;
            s.settings.start_lifetime = Range(1.0f, 3.0f);
            s.settings.start_speed = Range(0.5f, 2.0f);
            s.settings.gravity = 0.3f;
            s.settings.drag = 0.4f;
            s.settings.noise_strength = 2.0f;
            s.settings.orbital = 0.8f;
            s.settings.collide = true;
            s.settings.bounce = 0.4f;
            s.settings.ground_height = -1.0f;
        };
        add_system(ps, "big", glm::vec3(0, 0, 2), cfg);
        for (int i = 0; i < 12; ++i) {
            add_system(ps, "small_" + std::to_string(i), glm::vec3(i * 2.0f, 5, 1), [](ParticleSystem& s) {
                s.settings.rate = 60.0f;
                s.settings.noise_strength = 1.0f;
            });
        }
        if (jobs) ps.scene->set_job_engine(jobs);
        start(ps);
        ps.runner->set_force_serial(serial);
        run(ps, 1.5f);
        return ps;
    };
    coopa::job::JobEngine jobs(8);
    PScene a = build(true, nullptr);
    PScene b = build(false, &jobs);
    const auto& sa = a.runner->systems();
    const auto& sb = b.runner->systems();
    bool same = sa.size() == sb.size();
    for (size_t i = 0; same && i < sa.size(); ++i) {
        same &= sa[i]->pool().pos == sb[i]->pool().pos && sa[i]->pool().age == sb[i]->pool().age &&
                sa[i]->pool().vel == sb[i]->pool().vel;
    }
    expect(sa.size() == 13u && sa[0]->particle_count() > k_parallel_particles,
           "parallel: the big system is past the per-particle split threshold (" +
               std::to_string(sa.empty() ? 0u : sa[0]->particle_count()) + ")");
    expect(same, "parallel: 8 workers give bit-identical particles to the serial path");
    sa[0]->prepare_render(ParticleView{glm::vec3(0, -10, 3)});
    toy::particles::ParallelFor par = [&jobs](std::size_t n, const toy::particles::RangeFn& fn) {
        jobs.parallel_for_blocking(n, 0, [&fn](std::size_t b, std::size_t e) { fn(b, e); });
    };
    sb[0]->prepare_render(ParticleView{glm::vec3(0, -10, 3)}, &par);
    bool same_inst = sa[0]->instances().size() == sb[0]->instances().size();
    for (size_t i = 0; same_inst && i < sa[0]->instances().size(); ++i) {
        same_inst &= std::memcmp(&sa[0]->instances()[i], &sb[0]->instances()[i], sizeof(toy::render::ParticleInstance)) == 0;
    }
    expect(same_inst, "parallel: render prep is identical too, sort order included");
}
